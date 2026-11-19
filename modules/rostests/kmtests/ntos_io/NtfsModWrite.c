/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     NTFS modified page writer acquisition while a writer owns the file
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

/*
 * The modified page writer acquires a file through the file system's
 * AcquireForModWrite fast I/O callback before it writes dirty pages back,
 * and it never waits: a refusal leaves the pages dirty. A file system that
 * has a paging I/O resource takes that one, shared, for pages below the
 * valid data length, so a thread holding the main resource of the file -
 * any cached write does - doesn't keep the file's pages from being written
 * out. FsRtlAcquireFileForModWriteEx calls the same callback.
 */

#define FILE_SIZE   (16 * PAGE_SIZE)

typedef struct _HOLDER
{
    PERESOURCE Resource;
    KEVENT Held;
    KEVENT Release;
} HOLDER, *PHOLDER;

static
VOID
NTAPI
HoldMainResource(PVOID Context)
{
    PHOLDER Holder = Context;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(Holder->Resource, TRUE);
    KeSetEvent(&Holder->Held, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(&Holder->Release, Executive, KernelMode, FALSE, NULL);
    ExReleaseResourceLite(Holder->Resource);
    KeLeaveCriticalRegion();
}

/* Do what the modified page writer does for the page ending at EndingOffset:
 * acquire without waiting at APC_LEVEL, then release what was acquired. */
static
NTSTATUS
AcquireForModWrite(PFILE_OBJECT FileObject,
                   LONGLONG EndingOffset,
                   PERESOURCE *Acquired)
{
    PDEVICE_OBJECT Device = IoGetRelatedDeviceObject(FileObject);
    PFAST_IO_DISPATCH FastIo = Device->DriverObject->FastIoDispatch;
    LARGE_INTEGER Ending;
    PERESOURCE Resource = NULL;
    NTSTATUS Status;
    KIRQL OldIrql;

    *Acquired = NULL;
    Ending.QuadPart = EndingOffset;
    KeRaiseIrql(APC_LEVEL, &OldIrql);
    Status = FastIo->AcquireForModWrite(FileObject, &Ending, &Resource, Device);
    if (Status == STATUS_SUCCESS)
    {
        *Acquired = Resource;
        if (FastIo->ReleaseForModWrite == NULL ||
            !NT_SUCCESS(FastIo->ReleaseForModWrite(FileObject, Resource, Device)))
        {
            if (Resource != NULL)
                ExReleaseResourceLite(Resource);
        }
    }
    KeLowerIrql(OldIrql);
    return Status;
}

START_TEST(NtfsModWrite)
{
    WCHAR Root[MAX_PATH], Path[MAX_PATH];
    HANDLE Handle = NULL;
    PFILE_OBJECT FileObject = NULL;
    PFSRTL_COMMON_FCB_HEADER Header;
    PFAST_IO_DISPATCH FastIo;
    PUCHAR Buffer;
    ULONG_PTR Written;
    PERESOURCE Acquired;
    HOLDER Holder;
    PKTHREAD Thread;
    LARGE_INTEGER Timeout;
    SIZE_T Mismatch;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL))
        return;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"KMT_NTFS_MODWRITE.bin");
    Buffer = NtfsKmtAlloc(FILE_SIZE);
    if (!Buffer)
        return;
    NtfsKmtFillPattern(Buffer, FILE_SIZE, 118, 0);

    /* A cached file whose data is all written and initialized on disk */
    NtfsKmtDelete(Path);
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE | DELETE,
                           FILE_ATTRIBUTE_NORMAL, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE,
                           NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Status = NtfsKmtWrite(Handle, 0, Buffer, FILE_SIZE, &Written);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Written, FILE_SIZE);
    Status = NtfsKmtFlush(Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = ObReferenceObjectByHandle(Handle, 0, *IoFileObjectType, KernelMode,
                                       (PVOID *)&FileObject, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        FileObject = NULL;
        goto Cleanup;
    }

    FastIo = IoGetRelatedDeviceObject(FileObject)->DriverObject->FastIoDispatch;
    if (FastIo == NULL ||
        FastIo->SizeOfFastIoDispatch <= FIELD_OFFSET(FAST_IO_DISPATCH, AcquireForModWrite) ||
        FastIo->AcquireForModWrite == NULL)
    {
        skip(FALSE, "The file system has no AcquireForModWrite callback\n");
        goto Cleanup;
    }

    Header = FileObject->FsContext;
    ok(Header != NULL, "No FCB header\n");
    if (Header == NULL || Header->Resource == NULL)
    {
        skip(FALSE, "The file has no main resource\n");
        goto Cleanup;
    }
    ok(Header->PagingIoResource != NULL, "The file has no paging I/O resource\n");
    ok(Header->ValidDataLength.QuadPart >= FILE_SIZE,
       "ValidDataLength %I64d, expected at least %lu\n", Header->ValidDataLength.QuadPart, FILE_SIZE);

    /* Nobody owns the file */
    Status = AcquireForModWrite(FileObject, PAGE_SIZE, &Acquired);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Another thread owns the main resource, the way a cached write does.
     * Pages that are already valid can still be written out, through the
     * paging I/O resource. */
    Holder.Resource = Header->Resource;
    KeInitializeEvent(&Holder.Held, NotificationEvent, FALSE);
    KeInitializeEvent(&Holder.Release, NotificationEvent, FALSE);
    Thread = KmtStartThread(HoldMainResource, &Holder);
    if (Thread == NULL)
        goto Cleanup;
    Timeout.QuadPart = -10 * 1000 * 1000 * 10LL;
    Status = KeWaitForSingleObject(&Holder.Held, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (Status == STATUS_SUCCESS)
    {
        Status = AcquireForModWrite(FileObject, PAGE_SIZE, &Acquired);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (Status == STATUS_SUCCESS)
        {
            ok(Acquired != Header->Resource,
               "Got the main resource %p that another thread owns exclusively\n", Acquired);
        }

        Status = AcquireForModWrite(FileObject, FILE_SIZE, &Acquired);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    KmtFinishThread(Thread, &Holder.Release);

    /* And the file is still usable afterwards */
    RtlZeroMemory(Buffer, FILE_SIZE);
    Status = NtfsKmtRead(Handle, 0, Buffer, FILE_SIZE, &Written);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Written, FILE_SIZE);
    Mismatch = NtfsKmtCheckPattern(Buffer, FILE_SIZE, 118, 0);
    ok(Mismatch == (SIZE_T)-1, "Data differs at %Iu\n", Mismatch);

Cleanup:
    if (FileObject)
        ObDereferenceObject(FileObject);
    if (Handle)
    {
        FILE_DISPOSITION_INFORMATION Disposition = { TRUE };
        NtfsKmtSetInfo(Handle, &Disposition, sizeof(Disposition), FileDispositionInformation);
        ZwClose(Handle);
    }
    NtfsKmtFree(Buffer);
}
