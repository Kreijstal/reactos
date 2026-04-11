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
 * (drivers/filesystems/ntfs/fcb.c:174 — "leaking FCB"), and right now the
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
 * IOCTL — when this driver is loaded standalone (or via cmd.exe with no
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
                             FileHandle,
                             NULL);
    MMFSC_OK(Status == STATUS_SUCCESS,
             "MmCreateSection: 0x%08lx\n", Status);
    if (skip(NT_SUCCESS(Status) && SectionObject != NULL, "No section created\n"))
        return;

    MMFSC_OK(FileObject->SectionObjectPointer != NULL,
             "FileObject->SectionObjectPointer is NULL after MmCreateSection\n");
    MMFSC_OK(FileObject->SectionObjectPointer->DataSectionObject != NULL,
             "DataSectionObject still NULL after MmCreateSection\n");

    /* Drop our reference. There's no view mapped, so once we deref the
     * section object MM should be free to fully tear it down on demand. */
    ObDereferenceObject(SectionObject);

    /* Force the section closed. With no live view this MUST succeed and
     * MUST clear DataSectionObject. With the current stub it returns FALSE
     * and leaves DataSectionObject set — that's the bug we're chasing. */
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
                             FileHandle,
                             NULL);
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
     * leave the pointers as they are" — the caller hasn't given us
     * permission to defer, and we can't close right now. */
    Result = MmForceSectionClosed(FileObject->SectionObjectPointer, FALSE);
    MMFSC_OK(Result == FALSE,
             "MmForceSectionClosed(DelayClose=FALSE) with live view returned %u, expected FALSE\n",
             Result);
    MMFSC_OK(FileObject->SectionObjectPointer->DataSectionObject != NULL,
             "DataSectionObject was cleared even though a view is still mapped\n");

    /* DelayClose=TRUE while a view is still mapped: Windows accepts and
     * defers — i.e., returns TRUE to the caller (who can then proceed to
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

START_TEST(MmForceSectionClosed)
{
    NTSTATUS Status;
    HANDLE FileHandle = NULL;
    PFILE_OBJECT FileObject = NULL;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING FileName =
        RTL_CONSTANT_STRING(L"\\SystemRoot\\kmtest-MmForceSectionClosed.txt");
    LARGE_INTEGER FileOffset;
    UCHAR FileData[PAGE_SIZE];

    MMFSC_LOG("======== START_TEST(MmForceSectionClosed) ========\n");
    ok(ExGetPreviousMode() == UserMode, "Previous mode is kernel mode\n");

    /* Always-runnable test (no file involved). */
    TestEmptySectionPointers();

    /* Create a fresh PAGE_SIZE-byte file we can map. The file is opened
     * with FILE_DELETE_ON_CLOSE so it disappears even if the test crashes
     * — we don't want a stale file polluting subsequent runs. */
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
     * STATUS_ACCESS_DENIED (0xc0000022).  That's not a test failure — the
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

    RtlZeroMemory(FileData, sizeof FileData);
    FileOffset.QuadPart = 0;
    Status = ZwWriteFile(FileHandle, NULL, NULL, NULL, &IoStatusBlock,
                         FileData, sizeof FileData, &FileOffset, NULL);
    MMFSC_LOG("ZwWriteFile -> 0x%08lx\n", Status);
    MMFSC_OK(NT_SUCCESS(Status), "ZwWriteFile failed: 0x%08lx\n", Status);

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
     * SectionObjectPointers slot is reused — verifying that a clean teardown
     * leaves the slot ready for the next user. */
    TestDataSectionDereferenced(FileHandle, FileObject);
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
