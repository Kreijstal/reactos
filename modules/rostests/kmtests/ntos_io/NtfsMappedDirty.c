/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     Dirty mapped file pages the modified page writer may not write yet
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

/*
 * The modified page writer never waits for a file: when the file system
 * refuses AcquireForModWrite, the page just stays dirty and is retried
 * later. That must hold even when such pages are a large part of memory, as
 * long as there are still plenty of free pages: a refused write is not a sign
 * that the system has run out.
 *
 * The test extends a sparse file to 60% of the available memory, maps it,
 * holds the file's main and paging I/O resources exclusively, so that no
 * write-back can acquire the file, and dirties every page of the view. That is more than
 * half of what the system had at boot, and still leaves it hundreds of
 * megabytes free. It keeps the file that way for a while, then lets go and
 * checks that every change is still there and can be written back.
 *
 * It needs a volume with room for the file, so it skips on a small one, and
 * it runs for a few minutes on a guest with gigabytes of memory.
 */

#define CHUNK_SIZE      (1024 * 1024)
#define HOLD_SECONDS    10
#define MARKER(Page)    (0x119D1A7Bu ^ (ULONG)(Page))

static
ULONG_PTR
AvailableMb(VOID)
{
    SYSTEM_PERFORMANCE_INFORMATION Info;
    NTSTATUS Status;

    Status = ZwQuerySystemInformation(SystemPerformanceInformation, &Info, sizeof(Info), NULL);
    if (!NT_SUCCESS(Status))
        return 0;
    return (ULONG_PTR)Info.AvailablePages / (CHUNK_SIZE / PAGE_SIZE);
}

START_TEST(NtfsMappedDirty)
{
    WCHAR Root[MAX_PATH], Path[MAX_PATH];
    FILE_FS_SIZE_INFORMATION SizeInfo;
    FILE_END_OF_FILE_INFORMATION EndOfFile;
    IO_STATUS_BLOCK IoStatus;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle = NULL, Section = NULL;
    PFILE_OBJECT FileObject = NULL;
    PFSRTL_COMMON_FCB_HEADER Header = NULL;
    PUCHAR View = NULL;
    SIZE_T ViewSize = 0;
    ULONGLONG FileSize, FreeBytes;
    ULONG_PTR AvailableBefore, AvailableDirty;
    ULONG Pages, Page, Bad;
    LARGE_INTEGER Delay;
    BOOLEAN Held = FALSE;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL))
        return;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Root, L"KMT_NTFS_MAPPED_DIRTY.bin");
    NtfsKmtDelete(Path);

    AvailableBefore = AvailableMb();
    FileSize = (ULONGLONG)AvailableBefore * CHUNK_SIZE / 10 * 6;
    FileSize &= ~(ULONGLONG)(CHUNK_SIZE - 1);
    ok(FileSize != 0, "Can't tell the available memory\n");
    if (FileSize == 0)
        return;

    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE | DELETE,
                           FILE_ATTRIBUTE_NORMAL, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE,
                           NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = ZwQueryVolumeInformationFile(Handle, &IoStatus, &SizeInfo, sizeof(SizeInfo),
                                          FileFsSizeInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    FreeBytes = (ULONGLONG)SizeInfo.AvailableAllocationUnits.QuadPart *
                SizeInfo.SectorsPerAllocationUnit * SizeInfo.BytesPerSector;
    if (FreeBytes < FileSize + 32 * CHUNK_SIZE)
    {
        skip(FALSE, "Needs %I64u MB free on the NTFS volume, has %I64u MB\n",
             (FileSize + 32 * CHUNK_SIZE) >> 20, FreeBytes >> 20);
        goto Cleanup;
    }
    DbgPrint("NtfsMappedDirty: %Iu MB available, mapping %I64u MB\n",
             AvailableBefore, FileSize >> 20);

    /* A sparse file, so that extending it writes nothing and takes no
     * memory: the pages read as zeroes until they are dirtied */
    Status = NtfsKmtFsctl(Handle, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    EndOfFile.EndOfFile.QuadPart = FileSize;
    Status = NtfsKmtSetInfo(Handle, &EndOfFile, sizeof(EndOfFile), FileEndOfFileInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    DbgPrint("NtfsMappedDirty: extended, %Iu MB available\n", AvailableMb());

    Status = ObReferenceObjectByHandle(Handle, 0, *IoFileObjectType, KernelMode,
                                       (PVOID *)&FileObject, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        FileObject = NULL;
        goto Cleanup;
    }
    Header = FileObject->FsContext;
    if (Header == NULL || Header->Resource == NULL || Header->PagingIoResource == NULL)
    {
        skip(FALSE, "The file lacks a main or paging I/O resource\n");
        goto Cleanup;
    }

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateSection(&Section, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
                             &ObjectAttributes, NULL, PAGE_READWRITE, SEC_COMMIT, Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        Section = NULL;
        goto Cleanup;
    }
    Status = ZwMapViewOfSection(Section, ZwCurrentProcess(), (PVOID *)&View, 0, 0, NULL,
                                &ViewSize, ViewUnmap, 0, PAGE_READWRITE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        View = NULL;
        goto Cleanup;
    }
    ok(ViewSize >= FileSize, "View is %Iu bytes, expected %I64u\n", ViewSize, FileSize);
    Pages = (ULONG)(FileSize / PAGE_SIZE);

    /* Nobody can write this file's pages back now, whatever the offset. A
     * page fault in the view can still read, since a thread may acquire its
     * own resources again. */
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(Header->Resource, TRUE);
    ExAcquireResourceExclusiveLite(Header->PagingIoResource, TRUE);
    Held = TRUE;

    KmtStartSeh()
        for (Page = 0; Page < Pages; Page++)
        {
            *(volatile ULONG *)(View + (SIZE_T)Page * PAGE_SIZE) = MARKER(Page);
            if (Page % 65536 == 65535)
                DbgPrint("NtfsMappedDirty: %lu MB dirty, %Iu MB available\n", (Page + 1) / 256, AvailableMb());
        }
    KmtEndSeh(STATUS_SUCCESS);

    /* Much of the memory is now dirty and can't be written, and much of it
     * is still free. Give the balancer a few of its periods to look at it. */
    AvailableDirty = AvailableMb();
    DbgPrint("NtfsMappedDirty: dirtied, %Iu MB available\n", AvailableDirty);
    ok(AvailableDirty >= 64, "Only %Iu MB available with the file dirty\n", AvailableDirty);
    Delay.QuadPart = -10LL * 1000 * 1000 * HOLD_SECONDS;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);

    ExReleaseResourceLite(Header->PagingIoResource);
    ExReleaseResourceLite(Header->Resource);
    KeLeaveCriticalRegion();
    Held = FALSE;
    DbgPrint("NtfsMappedDirty: released, %Iu MB available\n", AvailableMb());

    /* Every page kept its change */
    Bad = 0;
    KmtStartSeh()
        for (Page = 0; Page < Pages; Page++)
        {
            if (*(volatile ULONG *)(View + (SIZE_T)Page * PAGE_SIZE) != MARKER(Page))
                Bad++;
        }
    KmtEndSeh(STATUS_SUCCESS);
    ok(Bad == 0, "%lu of %lu pages lost their change in the view\n", Bad, Pages);

    /* And can be written now */
    {
        PVOID Base = View;
        SIZE_T Size = ViewSize;

        Status = ZwFlushVirtualMemory(ZwCurrentProcess(), &Base, &Size, &IoStatus);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    Status = ZwUnmapViewOfSection(ZwCurrentProcess(), View);
    ok_eq_hex(Status, STATUS_SUCCESS);
    View = NULL;
    ZwClose(Section);
    Section = NULL;
    DbgPrint("NtfsMappedDirty: flushed, %Iu MB available\n", AvailableMb());

Cleanup:
    if (Held)
    {
        ExReleaseResourceLite(Header->PagingIoResource);
        ExReleaseResourceLite(Header->Resource);
        KeLeaveCriticalRegion();
    }
    if (View)
        ZwUnmapViewOfSection(ZwCurrentProcess(), View);
    if (Section)
        ZwClose(Section);
    if (FileObject)
        ObDereferenceObject(FileObject);
    if (Handle)
    {
        FILE_DISPOSITION_INFORMATION Disposition = { TRUE };
        NtfsKmtSetInfo(Handle, &Disposition, sizeof(Disposition), FileDispositionInformation);
        ZwClose(Handle);
    }
    DbgPrint("NtfsMappedDirty: done, %Iu MB available\n", AvailableMb());
}
