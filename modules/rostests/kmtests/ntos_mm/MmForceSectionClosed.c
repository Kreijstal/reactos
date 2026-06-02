/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Kernel-Mode Test Suite for MmForceSectionClosed
 * COPYRIGHT:   Copyright 2026 Kreijstal
 *
 * Why this test exists
 * --------------------
 * MmForceSectionClosed is the API a file system driver calls during FCB
 * teardown to release any cached/mapped sections backed by the file. The
 * ReactOS NTFS driver leaks an FCB whenever this call returns FALSE
 * (drivers/filesystems/ntfs/fcb.c:174 - "leaking FCB"), and right now the
 * function in ntoskrnl/mm/ARM3/section.c:2679 is a literal stub:
 *     UNIMPLEMENTED;
 *     return FALSE;
 * which is the root cause of the FCB leaks observed during the second
 * boot of a freshly NTFS-installed ReactOS (Kreijstal/reactos#14
 * "adjacent issue" called out in the bug text).
 *
 * What we test
 * ------------
 * 1. Pointer with no sections at all → trivially TRUE, nothing to do.
 * 2. Data section created and dereferenced (no live view) →
 *    SectionObjectPointers->DataSectionObject must end up NULL and the
 *    call must return TRUE.
 * 3. Data section with a still-mapped view, DelayClose = FALSE →
 *    cannot close, must return FALSE, pointers untouched.
 * 4. Data section with a still-mapped view, DelayClose = TRUE →
 *    Windows behaviour: returns TRUE, defers the actual teardown until
 *    the last reference goes away. We accept TRUE on Windows; on
 *    ReactOS we accept the same to track parity.
 *
 * The first time these run with the unimplemented stub, every test that
 * expects a clear/TRUE outcome should fail; that's the proof the harness
 * sees the bug. After the implementation lands they should all pass.
 */

#include <kmt_test.h>

/* The kmt_test ResultBuffer only goes back to the kmtest_.exe loader via
 * IOCTL - when this driver is loaded standalone (or via cmd.exe with no
 * good way to capture its stdout) we need a second channel to read the
 * results from outside the VM.  Mirror every assertion to DbgPrint so
 * each one shows up on the COM1 serial log we already capture.  The
 * "MMFSC: " prefix lets us grep for this test specifically. */
#define MMFSC_LOG(fmt, ...)  DbgPrint("MMFSC: " fmt, ##__VA_ARGS__)
#define MMFSC_OK(cond, fmt, ...) do {                                       \
    BOOLEAN _c = (BOOLEAN)(cond);                                           \
    MMFSC_LOG("[%s] " fmt, _c ? "PASS" : "FAIL", ##__VA_ARGS__);            \
    ok(_c, fmt, ##__VA_ARGS__);                                             \
} while (0)

static
VOID
TestEmptySectionPointers(VOID)
{
    SECTION_OBJECT_POINTERS Pointers = { 0 };
    BOOLEAN Result;

    MMFSC_LOG("=== TestEmptySectionPointers ===\n");

    /* No data/image/cache section attached → nothing to close → trivially TRUE.
     * Whatever the implementation does internally, this is the cheapest case
     * and should never report failure. */
    Result = MmForceSectionClosed(&Pointers, FALSE);
    MMFSC_OK(Result == TRUE,
             "Empty pointers, DelayClose=FALSE: returned %u, expected TRUE\n", Result);
    MMFSC_OK(Pointers.DataSectionObject == NULL,
             "DataSectionObject = %p, expected NULL\n", Pointers.DataSectionObject);
    MMFSC_OK(Pointers.ImageSectionObject == NULL,
             "ImageSectionObject = %p, expected NULL\n", Pointers.ImageSectionObject);
    MMFSC_OK(Pointers.SharedCacheMap == NULL,
             "SharedCacheMap = %p, expected NULL\n", Pointers.SharedCacheMap);

    Result = MmForceSectionClosed(&Pointers, TRUE);
    MMFSC_OK(Result == TRUE,
             "Empty pointers, DelayClose=TRUE: returned %u, expected TRUE\n", Result);
}

static
VOID
TestDataSectionDereferenced(
    IN HANDLE FileHandle,
    IN PFILE_OBJECT FileObject)
{
    NTSTATUS Status;
    PVOID SectionObject = NULL;
    LARGE_INTEGER MaximumSize;
    LARGE_INTEGER SectionOffset;
    PVOID BaseAddress = NULL;
    SIZE_T ViewSize = 0;
    LARGE_INTEGER ReaperDelay;
    BOOLEAN Result;

    MMFSC_LOG("=== TestDataSectionDereferenced ===\n");

    /* Create a data section backed by the file. The file's
     * SectionObjectPointer->DataSectionObject is wired up by MmCreateSection. */
    MaximumSize.QuadPart = 1;
    Status = MmCreateSection(&SectionObject,
                             SECTION_ALL_ACCESS,
                             NULL,
                             &MaximumSize,
                             PAGE_READWRITE,
                             SEC_COMMIT,
                             NULL,
                             FileObject);
    MMFSC_OK(Status == STATUS_SUCCESS,
             "MmCreateSection: 0x%08lx\n", Status);
    if (skip(NT_SUCCESS(Status) && SectionObject != NULL, "No section created\n"))
        return;

    MMFSC_OK(FileObject->SectionObjectPointer != NULL,
             "FileObject->SectionObjectPointer is NULL after MmCreateSection\n");
    MMFSC_OK(FileObject->SectionObjectPointer->DataSectionObject != NULL,
             "DataSectionObject still NULL after MmCreateSection\n");

    SectionOffset.QuadPart = 0;
    Status = MmMapViewOfSection(SectionObject,
                                PsGetCurrentProcess(),
                                &BaseAddress,
                                0, 0,
                                &SectionOffset,
                                &ViewSize,
                                ViewUnmap, 0,
                                PAGE_READWRITE);
    MMFSC_OK(Status == STATUS_SUCCESS,
             "MmMapViewOfSection: 0x%08lx\n", Status);
    if (skip(NT_SUCCESS(Status) && BaseAddress != NULL, "View not mapped\n"))
    {
        ObDereferenceObject(SectionObject);
        return;
    }

    *(volatile UCHAR *)BaseAddress = 0x5a;

    Status = MmUnmapViewOfSection(PsGetCurrentProcess(), BaseAddress);
    MMFSC_OK(Status == STATUS_SUCCESS,
             "MmUnmapViewOfSection: 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(SectionObject);
        return;
    }

    /* Drop our reference. The view is gone, but the faulted-in data page can
     * keep the segment attached to the file until MmForceSectionClosed purges
     * it. Give the object reaper a chance to run if deletion was deferred. */
    ObDereferenceObject(SectionObject);
    ReaperDelay.QuadPart = -10 * 1000 * 10;
    KeDelayExecutionThread(KernelMode, FALSE, &ReaperDelay);

    /* Force the section closed. With no live view this MUST succeed and
     * MUST clear DataSectionObject. With the current stub it returns FALSE
     * and leaves DataSectionObject set - that's the bug we're chasing. */
    Result = MmForceSectionClosed(FileObject->SectionObjectPointer, FALSE);
    MMFSC_OK(Result == TRUE,
             "MmForceSectionClosed returned %u, expected TRUE (no live view)\n", Result);
    MMFSC_OK(FileObject->SectionObjectPointer->DataSectionObject == NULL,
             "DataSectionObject = %p after force-close, expected NULL\n",
             FileObject->SectionObjectPointer->DataSectionObject);
}

static
VOID
TestDataSectionWithLiveView(
    IN HANDLE FileHandle,
    IN PFILE_OBJECT FileObject)
{
    NTSTATUS Status;
    PVOID SectionObject = NULL;
    LARGE_INTEGER MaximumSize;
    LARGE_INTEGER SectionOffset;
    PVOID BaseAddress = NULL;
    SIZE_T ViewSize = 0;
    BOOLEAN Result;

    MMFSC_LOG("=== TestDataSectionWithLiveView ===\n");

    MaximumSize.QuadPart = PAGE_SIZE;
    Status = MmCreateSection(&SectionObject,
                             SECTION_ALL_ACCESS,
                             NULL,
                             &MaximumSize,
                             PAGE_READWRITE,
                             SEC_COMMIT,
                             NULL,
                             FileObject);
    MMFSC_OK(Status == STATUS_SUCCESS,
             "MmCreateSection: 0x%08lx\n", Status);
    if (skip(NT_SUCCESS(Status) && SectionObject != NULL, "No section created\n"))
        return;

    SectionOffset.QuadPart = 0;
    Status = MmMapViewOfSection(SectionObject,
                                PsGetCurrentProcess(),
                                &BaseAddress,
                                0, 0,
                                &SectionOffset,
                                &ViewSize,
                                ViewUnmap, 0,
                                PAGE_READWRITE);
    MMFSC_OK(Status == STATUS_SUCCESS,
             "MmMapViewOfSection: 0x%08lx\n", Status);
    if (skip(NT_SUCCESS(Status) && BaseAddress != NULL, "View not mapped\n"))
    {
        ObDereferenceObject(SectionObject);
        return;
    }

    /* DelayClose=FALSE while a view is still mapped: contract is "fail and
     * leave the pointers as they are" - the caller hasn't given us
     * permission to defer, and we can't close right now. */
    Result = MmForceSectionClosed(FileObject->SectionObjectPointer, FALSE);
    MMFSC_OK(Result == FALSE,
             "MmForceSectionClosed(DelayClose=FALSE) with live view returned %u, expected FALSE\n",
             Result);
    MMFSC_OK(FileObject->SectionObjectPointer->DataSectionObject != NULL,
             "DataSectionObject was cleared even though a view is still mapped\n");

    /* DelayClose=TRUE while a view is still mapped: Windows accepts and
     * defers - i.e., returns TRUE to the caller (who can then proceed to
     * tear down the FCB) and lets MM finish the actual close when the
     * last reference goes away. The current stub will fail this. */
    Result = MmForceSectionClosed(FileObject->SectionObjectPointer, TRUE);
    MMFSC_OK(Result == TRUE,
             "MmForceSectionClosed(DelayClose=TRUE) with live view returned %u, expected TRUE\n",
             Result);

    /* Clean up the view ourselves. Whatever the implementation chose to
     * do above, we still own the view and must drop it. */
    Status = MmUnmapViewOfSection(PsGetCurrentProcess(), BaseAddress);
    MMFSC_OK(Status == STATUS_SUCCESS,
             "MmUnmapViewOfSection: 0x%08lx\n", Status);

    ObDereferenceObject(SectionObject);
}

static
VOID
TestSystemViewSurvivesForceClose(
    IN HANDLE FileHandle,
    IN PFILE_OBJECT FileObject)
{
    NTSTATUS Status;
    PVOID SectionObject = NULL;
    LARGE_INTEGER MaximumSize;
    PVOID SystemView = NULL;
    SIZE_T ViewSize = 0;
    BOOLEAN Result;
    volatile PUCHAR View;
    UCHAR Seen;
    ULONG i;
    PVOID *Churn;
    UCHAR FileData[PAGE_SIZE];
    LARGE_INTEGER FileOffset;
    IO_STATUS_BLOCK IoStatusBlock;
    const ULONG ChurnCount = 512;

    MMFSC_LOG("=== TestSystemViewSurvivesForceClose ===\n");

    /* Regression for the installed-font invisible-text bug (NT10 NTFS install
     * rendered all UI text invisible). A font is a file-backed data section
     * mapped into SYSTEM space via MmMapViewInSystemSpace; win32k hands that
     * buffer to FreeType and keeps it mapped, then closes the font's file
     * handle. The file-object teardown runs MmForceSectionClosed, which used
     * to purge the segment's pages. A system-space view carries NO per-process
     * rmap, so MiPurgeDataSegmentForClose's "is anyone still using this page"
     * check (which only looks for user rmaps) saw none and freed the frame out
     * from under the live view; a later allocation zeroed it and FreeType read
     * empty glyf tables -> glyphSize.cx == 0 -> invisible glyphs.
     *
     * Note MmMapViewInSystemSpace does NOT bump Segment->SectionCount (it maps
     * the segment directly), so the old "SectionCount > 1" guard does not catch
     * this - exactly the gap the bug fell through. The invariant under test:
     * while a system-space view maps the segment, MmForceSectionClosed
     * (DelayClose=FALSE) must refuse to purge and the view's bytes must
     * survive a subsequent free-list churn. */

    /* Stamp a known marker across the whole page so a zeroed (reused) frame is
     * distinguishable from intact file content. */
    RtlFillMemory(FileData, sizeof FileData, 0xA5);
    FileOffset.QuadPart = 0;
    Status = ZwWriteFile(FileHandle, NULL, NULL, NULL, &IoStatusBlock,
                         FileData, sizeof FileData, &FileOffset, NULL);
    MMFSC_OK(NT_SUCCESS(Status), "ZwWriteFile marker: 0x%08lx\n", Status);

    /* Read-only file-backed section, just like a font. */
    MaximumSize.QuadPart = PAGE_SIZE;
    Status = MmCreateSection(&SectionObject,
                             SECTION_ALL_ACCESS,
                             NULL,
                             &MaximumSize,
                             PAGE_READONLY,
                             SEC_COMMIT,
                             NULL,
                             FileObject);
    MMFSC_OK(Status == STATUS_SUCCESS, "MmCreateSection: 0x%08lx\n", Status);
    if (skip(NT_SUCCESS(Status) && SectionObject != NULL, "No section created\n"))
        return;

    ViewSize = PAGE_SIZE;
    Status = MmMapViewInSystemSpace(SectionObject, &SystemView, &ViewSize);
    MMFSC_OK(Status == STATUS_SUCCESS, "MmMapViewInSystemSpace: 0x%08lx\n", Status);
    if (skip(NT_SUCCESS(Status) && SystemView != NULL, "System view not mapped\n"))
    {
        ObDereferenceObject(SectionObject);
        return;
    }

    /* Fault the page in through the view and confirm it shows the marker. */
    View = (volatile PUCHAR)SystemView;
    Seen = View[0];
    MMFSC_OK(Seen == 0xA5,
             "System view byte before close = 0x%02x, expected 0xA5\n", Seen);

    /* Drop the section handle while keeping the system view mapped - exactly
     * what win32k does after MmMapViewInSystemSpace. */
    ObDereferenceObject(SectionObject);
    SectionObject = NULL;

    /* The decisive contract check. With a live system-space view and
     * DelayClose=FALSE, MmForceSectionClosed must refuse (return FALSE) and
     * leave DataSectionObject set. Before the fix it purged the (rmap-invisible)
     * system view's pages and returned TRUE. */
    Result = MmForceSectionClosed(FileObject->SectionObjectPointer, FALSE);
    MMFSC_OK(Result == FALSE,
             "MmForceSectionClosed(DelayClose=FALSE) with live SYSTEM view returned %u, expected FALSE\n",
             Result);
    MMFSC_OK(FileObject->SectionObjectPointer->DataSectionObject != NULL,
             "DataSectionObject cleared while a system-space view is still mapped\n");

    /* Churn the free list: had the page been wrongly freed, these zeroing
     * allocations reclaim and wipe its frame, destroying the view's contents. */
    Churn = ExAllocatePoolWithTag(NonPagedPool, ChurnCount * sizeof(PVOID), 'CSFM');
    if (Churn != NULL)
    {
        for (i = 0; i < ChurnCount; i++)
        {
            Churn[i] = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, 'CSFM');
            if (Churn[i] != NULL)
                RtlFillMemory(Churn[i], PAGE_SIZE, 0x00);
        }
        for (i = 0; i < ChurnCount; i++)
            if (Churn[i] != NULL)
                ExFreePoolWithTag(Churn[i], 'CSFM');
        ExFreePoolWithTag(Churn, 'CSFM');
    }

    /* The data-integrity assertion: the live system view must still read the
     * marker. Before the fix this read 0x00 (frame freed and zeroed). */
    Seen = View[0];
    MMFSC_OK(Seen == 0xA5,
             "System view byte AFTER force-close + free-list churn = 0x%02x, expected 0xA5 "
             "(page freed out from under a live system-space view)\n", Seen);

    /* Release the live system view. This test's scope is the system-view
     * survival invariant above; the clean-close-after-teardown path (purge of
     * a no-longer-mapped data segment) is a separate invariant owned by
     * TestDataSectionDereferenced, so we do not re-assert it here - we just
     * drop the view so the file object can be released by the caller. */
    Status = MmUnmapViewInSystemSpace(SystemView);
    MMFSC_OK(Status == STATUS_SUCCESS, "MmUnmapViewInSystemSpace: 0x%08lx\n", Status);
}

START_TEST(MmForceSectionClosed)
{
    NTSTATUS Status;
    HANDLE FileHandle = NULL;
    PFILE_OBJECT FileObject = NULL;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING FileName =
        RTL_CONSTANT_STRING(L"\\SystemRoot\\kmtest-MmForceSectionClosed.txt");
    FILE_END_OF_FILE_INFORMATION EndOfFile;

    MMFSC_LOG("======== START_TEST(MmForceSectionClosed) ========\n");
    ok(ExGetPreviousMode() == UserMode, "Previous mode is kernel mode\n");

    /* Always-runnable test (no file involved). */
    TestEmptySectionPointers();

    /* Create a fresh PAGE_SIZE-byte file we can map. The file is opened
     * with FILE_DELETE_ON_CLOSE so it disappears even if the test crashes
     * - we don't want a stale file polluting subsequent runs. */
    InitializeObjectAttributes(&ObjectAttributes, &FileName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    Status = ZwCreateFile(&FileHandle,
                          GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                          &ObjectAttributes,
                          &IoStatusBlock,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_SUPERSEDE,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL, 0);
    MMFSC_LOG("ZwCreateFile -> 0x%08lx FileHandle=%p\n", Status, FileHandle);
    /* On a livecd \SystemRoot is read-only and ZwCreateFile returns
     * STATUS_ACCESS_DENIED (0xc0000022).  That's not a test failure - the
     * file-based subtests just can't run there.  Use skip() (not ok()) so
     * the harness reports them as skipped, not failed.  The integration
     * test against an installed (NTFS) system is what exercises the
     * file-backed paths anyway. */
    if (skip(NT_SUCCESS(Status) && FileHandle != NULL,
             "ZwCreateFile failed (0x%08lx) - likely read-only volume; "
             "file-backed sub-tests skipped\n", Status))
    {
        MMFSC_LOG("SKIP: file-backed sub-tests (no writable test file)\n");
        return;
    }

    /* Give the file a one-page size WITHOUT a cached write. A cached ZwWriteFile
     * here would leave a live Cc shared-cache-map view of the file's data
     * segment for the remainder of the test. That view lives in system space and
     * carries no per-process rmap, so it correctly blocks MmForceSectionClosed
     * from purging the segment - which would mask the clean-close path that
     * TestDataSectionDereferenced verifies (and previously let it "pass" only
     * because the purge wrongly freed the Cc-mapped page). Real FS teardown
     * (NtfsCleanup) drops the Cc view via CcUninitializeCacheMap before
     * MmForceSectionClosed runs; setting EOF directly models that precondition
     * without ever mapping a cache view. */
    EndOfFile.EndOfFile.QuadPart = PAGE_SIZE;
    Status = ZwSetInformationFile(FileHandle, &IoStatusBlock, &EndOfFile,
                                  sizeof(EndOfFile), FileEndOfFileInformation);
    MMFSC_LOG("ZwSetInformationFile(EOF) -> 0x%08lx\n", Status);
    MMFSC_OK(NT_SUCCESS(Status), "ZwSetInformationFile(EOF): 0x%08lx\n", Status);

    Status = ObReferenceObjectByHandle(FileHandle,
                                       FILE_READ_DATA | FILE_WRITE_DATA,
                                       *IoFileObjectType, KernelMode,
                                       (PVOID *)&FileObject, NULL);
    MMFSC_LOG("ObReferenceObjectByHandle -> 0x%08lx FileObject=%p\n", Status, FileObject);
    MMFSC_OK(Status == STATUS_SUCCESS, "ObReferenceObjectByHandle: 0x%08lx\n", Status);
    if (skip(NT_SUCCESS(Status) && FileObject != NULL,
             "Failed to reference FileObject\n"))
    {
        MMFSC_LOG("SKIP: cannot continue without FileObject\n");
        ZwClose(FileHandle);
        return;
    }

    /* Each sub-test creates its own section against the same file, so the
     * SectionObjectPointers slot is reused - verifying that a clean teardown
     * leaves the slot ready for the next user. */
    TestDataSectionDereferenced(FileHandle, FileObject);
    TestSystemViewSurvivesForceClose(FileHandle, FileObject);
    TestDataSectionWithLiveView(FileHandle, FileObject);

    ObDereferenceObject(FileObject);

    /* Re-open with FILE_DELETE_ON_CLOSE to clean up the file. */
    {
        HANDLE DeleteHandle = NULL;
        InitializeObjectAttributes(&ObjectAttributes, &FileName,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        Status = ZwCreateFile(&DeleteHandle, DELETE, &ObjectAttributes,
                              &IoStatusBlock, NULL, FILE_ATTRIBUTE_NORMAL,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              FILE_OPEN,
                              FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE,
                              NULL, 0);
        if (NT_SUCCESS(Status) && DeleteHandle != NULL)
            ZwClose(DeleteHandle);
    }

    ZwClose(FileHandle);

    MMFSC_LOG("======== END_TEST(MmForceSectionClosed) ========\n");
}
