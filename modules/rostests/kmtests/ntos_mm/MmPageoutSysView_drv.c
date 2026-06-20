/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Regression test - the page-out trimmer must not free a clean
 *              SHARE_COUNT==0 segment page that a Cc system-space view still maps
 * COPYRIGHT:   Copyright 2026 Kreijstal
 *
 * Background
 * ----------
 * Cc makes a whole VACB stripe (256 KB) resident via MmMakeSegmentResident,
 * which installs each page at SHARE_COUNT == 0; a page only reaches SHARE_COUNT
 * 1 once its system VA (Vacb->BaseAddress) is faulted. A Cc system-space view
 * has Process == NULL, so it carries no per-process rmap. So an untouched page
 * in a freshly CcMapData'd stripe sits in a peculiar state: resident, clean,
 * SHARE_COUNT == 0, mapped by a system-space view with no rmap.
 *
 * The working-set trimmer (MmPageOutPhysicalAddress -> MmCheckDirtySegment with
 * PageOut) used to free such a page back to the free list - while the VACB
 * still maps it through Vacb->BaseAddress - causing intermittent page
 * double-use (rmap-dup ASSERT, CcMapData bugcheck, bad-PTE faults). The fix
 * requires Segment->SystemMapCount == 0 before that free.
 *
 * This test reproduces the exact state and pages out that one page via the
 * ntoskrnl test-support export MmTestPageOutSegmentOffset, then checks whether
 * the page survived.
 */

#include <kmt_test.h>

#define MMPV_LOG(fmt, ...)  DbgPrint("MMPV: " fmt, ##__VA_ARGS__)

#define IOCTL_START_TEST  1
#define IOCTL_FINISH_TEST 2

/* ntoskrnl regression-test support export (ntoskrnl.spec): pages out exactly
 * the segment's page at the offset (attaches to the System process internally)
 * and reports survival. 1 = retained (fix held), 0 = freed (bug), -1 = no
 * segment, -2 = page was not resident (setup invalid). */
LONG NTAPI MmTestPageOutSegmentOffset(PVOID DataSectionObject, LONGLONG Offset);

#ifndef VACB_MAPPING_GRANULARITY
#define VACB_MAPPING_GRANULARITY (0x40000)
#endif

/* A page within the first VACB that we never fault through BaseAddress, so it
 * stays resident at SHARE_COUNT == 0. */
#define WATCH_OFFSET (0x8000)

typedef struct _TEST_FCB
{
    FSRTL_ADVANCED_FCB_HEADER Header;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    FAST_MUTEX HeaderMutex;
} TEST_FCB, *PTEST_FCB;

static PFILE_OBJECT TestFileObject;
static PDEVICE_OBJECT TestDeviceObject;
static KMT_IRP_HANDLER TestIrpHandler;
static KMT_MESSAGE_HANDLER TestMessageHandler;

static CC_FILE_SIZES FileSizes = {
    RTL_CONSTANT_LARGE_INTEGER((LONGLONG)0x80000), /* AllocationSize 512 KB */
    RTL_CONSTANT_LARGE_INTEGER((LONGLONG)0x80000), /* FileSize */
    RTL_CONSTANT_LARGE_INTEGER((LONGLONG)0x80000)  /* ValidDataLength */
};

static BOOLEAN NTAPI NopAcquire(PVOID Ctx, BOOLEAN Wait) { UNREFERENCED_PARAMETER(Ctx); UNREFERENCED_PARAMETER(Wait); return TRUE; }
static VOID    NTAPI NopRelease(PVOID Ctx) { UNREFERENCED_PARAMETER(Ctx); }

static CACHE_MANAGER_CALLBACKS Callbacks = {
    NopAcquire, NopRelease, NopAcquire, NopRelease
};

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    *DeviceName = L"MmPageoutSysView";
    *Flags = TESTENTRY_NO_EXCLUSIVE_DEVICE |
             TESTENTRY_BUFFERED_IO_DEVICE |
             TESTENTRY_NO_READONLY_DEVICE;

    KmtRegisterIrpHandler(IRP_MJ_READ, NULL, TestIrpHandler);
    KmtRegisterMessageHandler(0, NULL, TestMessageHandler);
    return STATUS_SUCCESS;
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    PAGED_CODE();
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
            return NULL;

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
VOID
PerformTest(
    PDEVICE_OBJECT DeviceObject)
{
    PVOID Bcb = NULL;
    PVOID Buffer = NULL;
    PTEST_FCB Fcb;
    LARGE_INTEGER Offset;
    BOOLEAN Ret;
    PVOID DataSection;
    LONG Result;

    TestDeviceObject = DeviceObject;
    TestFileObject = IoCreateStreamFileObject(NULL, DeviceObject);
    if (skip(TestFileObject != NULL, "Failed to allocate FO\n"))
        return;

    Fcb = ExAllocatePool(NonPagedPool, sizeof(TEST_FCB));
    if (skip(Fcb != NULL, "ExAllocatePool failed\n"))
        return;

    RtlZeroMemory(Fcb, sizeof(TEST_FCB));
    ExInitializeFastMutex(&Fcb->HeaderMutex);
    FsRtlSetupAdvancedHeader(&Fcb->Header, &Fcb->HeaderMutex);
    TestFileObject->FsContext = Fcb;
    TestFileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;

    KmtStartSeh();
    CcInitializeCacheMap(TestFileObject, &FileSizes, FALSE, &Callbacks, NULL);
    KmtEndSeh(STATUS_SUCCESS);

    if (skip(CcIsFileCached(TestFileObject) == TRUE, "CcInitializeCacheMap failed\n"))
        return;

    /* Make the whole first VACB stripe resident WITHOUT touching its system VA.
     * CcMapData ensures the range resident (each page SHARE_COUNT == 0) and
     * returns the VACB base; we deliberately never dereference Buffer, so every
     * page stays at SHARE_COUNT == 0 while the VACB (system view) maps it. Keep
     * Bcb pinned across the page-out so SystemMapCount stays > 0. */
    Offset.QuadPart = 0;
    Ret = FALSE;
    KmtStartSeh();
    Ret = CcMapData(TestFileObject, &Offset, VACB_MAPPING_GRANULARITY, MAP_WAIT, &Bcb, &Buffer);
    KmtEndSeh(STATUS_SUCCESS);
    if (skip(Ret == TRUE, "CcMapData failed\n"))
        return;

    DataSection = Fcb->SectionObjectPointers.DataSectionObject;
    ok(DataSection != NULL, "DataSectionObject is NULL after CcMapData\n");
    if (DataSection == NULL)
    {
        CcUnpinData(Bcb);
        return;
    }

    /* Page out exactly the untouched watch page and report whether it survived. */
    Result = MmTestPageOutSegmentOffset(DataSection, WATCH_OFFSET);
    MMPV_LOG("MmTestPageOutSegmentOffset returned %ld\n", Result);

    ok(Result != -2, "Test setup invalid: watch page was not resident before page-out\n");
    ok(Result != -1, "Test setup invalid: no data section\n");
    if (Result >= 0)
    {
        ok(Result == 1,
           "Trimmer freed a clean SHARE_COUNT==0 page the Cc system-space view "
           "still maps (got %ld; 1=retained, 0=freed)\n", Result);
    }

    CcUnpinData(Bcb);
}

static
VOID
CleanupTest(VOID)
{
    LARGE_INTEGER Zero = RTL_CONSTANT_LARGE_INTEGER(0LL);
    CACHE_UNINITIALIZE_EVENT CacheUninitEvent;

    if (TestFileObject != NULL)
    {
        if (CcIsFileCached(TestFileObject))
        {
            KeInitializeEvent(&CacheUninitEvent.Event, NotificationEvent, FALSE);
            CcUninitializeCacheMap(TestFileObject, &Zero, &CacheUninitEvent);
            KeWaitForSingleObject(&CacheUninitEvent.Event, Executive, KernelMode, FALSE, NULL);
        }
        if (TestFileObject->FsContext != NULL)
        {
            ExFreePool(TestFileObject->FsContext);
            TestFileObject->FsContext = NULL;
            TestFileObject->SectionObjectPointer = NULL;
        }
        ObDereferenceObject(TestFileObject);
    }
    TestFileObject = NULL;
    TestDeviceObject = NULL;
}

static
NTSTATUS
TestMessageHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _In_opt_ PVOID Buffer,
    _In_ SIZE_T InLength,
    _Inout_ PSIZE_T OutLength)
{
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);
    UNREFERENCED_PARAMETER(OutLength);

    FsRtlEnterFileSystem();
    switch (ControlCode)
    {
        case IOCTL_START_TEST:  PerformTest(DeviceObject); break;
        case IOCTL_FINISH_TEST: CleanupTest(); break;
        default: break;
    }
    FsRtlExitFileSystem();
    return STATUS_SUCCESS;
}

static
NTSTATUS
TestIrpHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStack)
{
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(DeviceObject);

    FsRtlEnterFileSystem();
    Irp->IoStatus.Information = 0;

    if (IoStack->MajorFunction == IRP_MJ_READ)
    {
        PVOID Buffer;
        ULONG Length = IoStack->Parameters.Read.Length;

        Buffer = MapAndLockUserBuffer(Irp, Length);
        if (Buffer != NULL)
        {
            RtlFillMemory(Buffer, Length, 0xBA);
            Irp->IoStatus.Information = Length;
            Status = STATUS_SUCCESS;
        }
        else
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    FsRtlExitFileSystem();
    return Status;
}
