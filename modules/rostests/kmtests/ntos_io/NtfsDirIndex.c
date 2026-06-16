/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS directory $I30 B-Tree index
 * PROGRAMMER:      ReactOS Team
 *
 * Populates a directory on a writable NTFS volume with enough scattered-order
 * entries to force a non-resident $INDEX_ALLOCATION B-Tree with several node
 * splits, then verifies every create succeeds, every name is found afterwards,
 * and a FILE_CREATE of an existing name reports STATUS_OBJECT_NAME_COLLISION
 * rather than re-inserting a duplicate key (which trips the NtfsInsertKey
 * ASSERT(Comparison != 0) in btree.c and corrupts the directory index).
 */

#include <kmt_test.h>

#define FILE_COUNT       1500
#define NAME_BUFFER_CCH  64

/* Scatter insertion order the way a real archive extraction does, instead of
 * inserting in already-sorted order (which only ever appends to the rightmost
 * leaf and never exercises interior splits). 64-bit math is mandatory: a 32-bit
 * (i * 2654435761) overflows and stops being a bijection, fabricating duplicate
 * names. With gcd(2654435761,1500)==1 the 64-bit form is a true permutation. */
#define SCATTER(i)  ((ULONG)(((ULONGLONG)(i) * 2654435761ull) % FILE_COUNT))

static
BOOLEAN
FindWritableNtfsVolume(
    _Out_writes_(MAX_PATH) PWCHAR RootPath)
{
    NTSTATUS Status;
    WCHAR letter;
    WCHAR path[16];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE handle;
    UCHAR buffer[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 32 * sizeof(WCHAR)];
    PFILE_FS_ATTRIBUTE_INFORMATION fsAttr = (PFILE_FS_ATTRIBUTE_INFORMATION)buffer;

    for (letter = L'D'; letter <= L'Z'; letter++)
    {
        RtlStringCchPrintfW(path, RTL_NUMBER_OF(path), L"\\??\\%c:\\", letter);
        RtlInitUnicodeString(&name, path);
        InitializeObjectAttributes(&oa, &name,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        Status = ZwOpenFile(&handle,
                            FILE_LIST_DIRECTORY | SYNCHRONIZE,
                            &oa, &iosb,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
        if (!NT_SUCCESS(Status))
            continue;

        RtlZeroMemory(buffer, sizeof(buffer));
        Status = ZwQueryVolumeInformationFile(handle, &iosb, fsAttr,
                                              sizeof(buffer),
                                              FileFsAttributeInformation);
        ZwClose(handle);
        if (!NT_SUCCESS(Status))
            continue;

        if (fsAttr->FileSystemNameLength == 4 * sizeof(WCHAR) &&
            fsAttr->FileSystemName[0] == L'N' &&
            fsAttr->FileSystemName[1] == L'T' &&
            fsAttr->FileSystemName[2] == L'F' &&
            fsAttr->FileSystemName[3] == L'S')
        {
            RtlStringCchPrintfW(RootPath, MAX_PATH, L"\\??\\%c:\\", letter);
            trace("Found writable NTFS volume at %c:\n", (CHAR)letter);
            return TRUE;
        }
    }

    return FALSE;
}

static
NTSTATUS
CreateOne(
    _In_ PCWSTR FullPath,
    _In_ ULONG Disposition,
    _In_ ULONG Options,
    _Out_opt_ PULONG_PTR Information)
{
    NTSTATUS Status;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE handle;

    RtlInitUnicodeString(&name, FullPath);
    InitializeObjectAttributes(&oa, &name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    iosb.Status = 0xFFFFFFFF;
    iosb.Information = 0xFFFFFFFF;
    Status = ZwCreateFile(&handle,
                          GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                          &oa, &iosb, NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          Disposition,
                          Options | FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL, 0);
    if (Information != NULL)
        *Information = NT_SUCCESS(Status) ? iosb.Information : 0;
    if (NT_SUCCESS(Status))
        ZwClose(handle);
    return Status;
}

static
NTSTATUS
DeleteOne(
    _In_ PCWSTR FullPath)
{
    NTSTATUS Status;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE handle;

    FILE_DISPOSITION_INFORMATION disp;

    RtlInitUnicodeString(&name, FullPath);
    InitializeObjectAttributes(&oa, &name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    /* The canonical delete (what DeleteFileW / rmdir do): open with DELETE
     * access, set FileDispositionInformation.DeleteFile, close. The record and
     * its $I30 directory entry must be gone afterwards. */
    Status = ZwCreateFile(&handle,
                          DELETE | SYNCHRONIZE,
                          &oa, &iosb, NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          FILE_OPEN,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL, 0);
    if (!NT_SUCCESS(Status))
        return Status;

    disp.DeleteFile = TRUE;
    Status = ZwSetInformationFile(handle, &iosb, &disp, sizeof(disp),
                                  FileDispositionInformation);
    ZwClose(handle);
    return Status;
}

/* Open an existing file with FILE_DELETE_ON_CLOSE and close it: the record and
 * its $I30 entry must be gone afterwards (the alternate delete disposition,
 * relied on by cygwin/msys2 unlink and temp files). */
static
NTSTATUS
DeleteOnCloseOne(
    _In_ PCWSTR FullPath)
{
    NTSTATUS Status;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE handle;

    RtlInitUnicodeString(&name, FullPath);
    InitializeObjectAttributes(&oa, &name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    Status = ZwCreateFile(&handle,
                          DELETE | SYNCHRONIZE,
                          &oa, &iosb, NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          FILE_OPEN,
                          FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE |
                              FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL, 0);
    if (NT_SUCCESS(Status))
        ZwClose(handle);
    return Status;
}

static
VOID
NtfsDirIndexTest(VOID)
{
    NTSTATUS Status;
    WCHAR root[MAX_PATH];
    WCHAR dirPath[MAX_PATH];
    WCHAR filePath[MAX_PATH];
    ULONG i;
    ULONG created = 0, createFail = 0;
    ULONG missing = 0, collisionOk = 0, collisionBad = 0;

    if (!FindWritableNtfsVolume(root))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    RtlStringCchPrintfW(dirPath, RTL_NUMBER_OF(dirPath), L"%lskmt_ntfs_idx", root);

    /* Create the test directory (open if it survived a prior run). */
    Status = CreateOne(dirPath, FILE_OPEN_IF, FILE_DIRECTORY_FILE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* Remove any survivors of a previous run so the directory starts empty;
     * otherwise leftover entries make Phase 1's FILE_CREATE collide and mask
     * the real result. (The data disk is reused across runs.) */
    for (i = 0; i < FILE_COUNT; i++)
    {
        RtlStringCchPrintfW(filePath, RTL_NUMBER_OF(filePath),
                            L"%ls\\curs_%05u_indexfill.3x.gz", dirPath, i);
        DeleteOne(filePath);
    }

    /* Phase 1: populate, scattered order, every create must succeed. This is
     * what the msys2 extraction does to the ncurses man directory. A buggy
     * NtfsInsertKey trips ASSERT(Comparison != 0) here (recorded as a FAIL by
     * the kmtest SEH wrapper under FLG_DISABLE_DEBUG_PROMPTS). */
    for (i = 0; i < FILE_COUNT; i++)
    {
        RtlStringCchPrintfW(filePath, RTL_NUMBER_OF(filePath),
                            L"%ls\\curs_%05u_indexfill.3x.gz", dirPath, SCATTER(i));
        Status = CreateOne(filePath, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL);
        if (NT_SUCCESS(Status))
            created++;
        else
        {
            createFail++;
            if (createFail <= 5)
                trace("create #%lu (%05u) failed: 0x%lx\n", i, SCATTER(i), Status);
        }
    }
    trace("Phase 1: created %lu/%u files (%lu failures)\n",
          created, FILE_COUNT, createFail);
    ok(created == FILE_COUNT, "Expected %u created, got %lu\n", FILE_COUNT, created);

    /* Phase 2: every name must be found by an open (lookup must agree with the
     * inserts). A miss here means the index lookup descends the B-Tree wrong. */
    for (i = 0; i < FILE_COUNT; i++)
    {
        RtlStringCchPrintfW(filePath, RTL_NUMBER_OF(filePath),
                            L"%ls\\curs_%05u_indexfill.3x.gz", dirPath, i);
        Status = CreateOne(filePath, FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL);
        if (!NT_SUCCESS(Status))
        {
            missing++;
            if (missing <= 5)
                trace("lookup miss %05u: 0x%lx\n", i, Status);
        }
    }
    trace("Phase 2: %lu of %u names not found by lookup\n", missing, FILE_COUNT);
    ok(missing == 0, "Expected 0 lookup misses, got %lu\n", missing);

    /* Phase 3: FILE_CREATE of an existing name must report a collision, never
     * succeed (a success means lookup missed -> a duplicate key would be
     * inserted, exactly the install-time corruption). */
    for (i = 0; i < FILE_COUNT; i++)
    {
        RtlStringCchPrintfW(filePath, RTL_NUMBER_OF(filePath),
                            L"%ls\\curs_%05u_indexfill.3x.gz", dirPath, i);
        Status = CreateOne(filePath, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL);
        if (Status == STATUS_OBJECT_NAME_COLLISION)
            collisionOk++;
        else if (NT_SUCCESS(Status))
        {
            collisionBad++;
            if (collisionBad <= 5)
                trace("DUP-INSERT: FILE_CREATE of existing %05u SUCCEEDED\n", i);
        }
    }
    trace("Phase 3: %lu correct collisions, %lu wrong successes\n",
          collisionOk, collisionBad);
    ok(collisionBad == 0, "FILE_CREATE of existing name succeeded %lu times (duplicate key)\n",
       collisionBad);
    ok(collisionOk == FILE_COUNT, "Expected %u collisions, got %lu\n",
       FILE_COUNT, collisionOk);

    /* Phase 4: re-write every existing name with FILE_OVERWRITE_IF then
     * FILE_SUPERSEDE - this is what archive extraction does when the same path
     * is emitted twice (flattened hardlinks). Must OPEN the existing record and
     * NOT add a second $I30 key; a buggy path re-inserts and trips
     * NtfsInsertKey ASSERT(Comparison != 0) (the install-time crash). */
    {
        ULONG overOk = 0, overBad = 0, supOk = 0, supBad = 0;
        for (i = 0; i < FILE_COUNT; i++)
        {
            ULONG_PTR info = 0;
            RtlStringCchPrintfW(filePath, RTL_NUMBER_OF(filePath),
                                L"%ls\\curs_%05u_indexfill.3x.gz", dirPath, i);
            Status = CreateOne(filePath, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, &info);
            if (NT_SUCCESS(Status) && info == FILE_OVERWRITTEN)
                overOk++;
            else
            {
                overBad++;
                if (overBad <= 5)
                    trace("overwrite %05u: status=0x%lx info=%Iu\n", i, Status, info);
            }
        }
        for (i = 0; i < FILE_COUNT; i++)
        {
            ULONG_PTR info = 0;
            RtlStringCchPrintfW(filePath, RTL_NUMBER_OF(filePath),
                                L"%ls\\curs_%05u_indexfill.3x.gz", dirPath, i);
            Status = CreateOne(filePath, FILE_SUPERSEDE, FILE_NON_DIRECTORY_FILE, &info);
            if (NT_SUCCESS(Status) && info == FILE_SUPERSEDED)
                supOk++;
            else
            {
                supBad++;
                if (supBad <= 5)
                    trace("supersede %05u: status=0x%lx info=%Iu\n", i, Status, info);
            }
        }
        trace("Phase 4: overwrite ok=%lu bad=%lu; supersede ok=%lu bad=%lu\n",
              overOk, overBad, supOk, supBad);
        ok(overBad == 0, "FILE_OVERWRITE_IF must succeed and report FILE_OVERWRITTEN, failed %lu times\n", overBad);
        ok(supBad == 0, "FILE_SUPERSEDE must succeed and report FILE_SUPERSEDED, failed %lu times\n", supBad);
    }

    /* Phase 5: the install's real shape - rmdir /s /q then re-extract. Delete
     * every entry, confirm each is gone (lookup must MISS, proving the $I30 key
     * was actually removed), then re-create all in scattered order. A
     * dangling-$I30 producer leaves a stale key behind: the re-create then
     * either collides on a name whose record was deleted (wrong COLLISION) or
     * lookup-misses a live entry and inserts a duplicate key -> the btree.c
     * ASSERT(Comparison != 0). Several cycles shake the producer out. */
    {
        ULONG cycle;
        ULONG delFail = 0, stale = 0, recreFail = 0, recreMiss = 0;

        for (cycle = 0; cycle < 4; cycle++)
        {
            for (i = 0; i < FILE_COUNT; i++)
            {
                RtlStringCchPrintfW(filePath, RTL_NUMBER_OF(filePath),
                                    L"%ls\\curs_%05u_indexfill.3x.gz", dirPath, i);
                Status = DeleteOne(filePath);
                if (!NT_SUCCESS(Status))
                {
                    delFail++;
                    if (delFail <= 5)
                        trace("cycle %lu delete %05u: 0x%lx\n", cycle, i, Status);
                }
            }
            /* every name must now be gone */
            for (i = 0; i < FILE_COUNT; i++)
            {
                RtlStringCchPrintfW(filePath, RTL_NUMBER_OF(filePath),
                                    L"%ls\\curs_%05u_indexfill.3x.gz", dirPath, i);
                Status = CreateOne(filePath, FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL);
                if (NT_SUCCESS(Status))
                {
                    stale++;
                    if (stale <= 5)
                        trace("cycle %lu STALE: %05u still openable after delete\n", cycle, i);
                }
            }
            /* re-extract: re-create all in scattered order */
            for (i = 0; i < FILE_COUNT; i++)
            {
                RtlStringCchPrintfW(filePath, RTL_NUMBER_OF(filePath),
                                    L"%ls\\curs_%05u_indexfill.3x.gz", dirPath, SCATTER(i));
                Status = CreateOne(filePath, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL);
                if (!NT_SUCCESS(Status))
                {
                    recreFail++;
                    if (recreFail <= 5)
                        trace("cycle %lu re-create %05u: 0x%lx\n", cycle, SCATTER(i), Status);
                }
            }
            /* and every re-created name must be found again */
            for (i = 0; i < FILE_COUNT; i++)
            {
                RtlStringCchPrintfW(filePath, RTL_NUMBER_OF(filePath),
                                    L"%ls\\curs_%05u_indexfill.3x.gz", dirPath, i);
                Status = CreateOne(filePath, FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL);
                if (!NT_SUCCESS(Status))
                {
                    recreMiss++;
                    if (recreMiss <= 5)
                        trace("cycle %lu re-lookup miss %05u: 0x%lx\n", cycle, i, Status);
                }
            }
            trace("Phase 5 cycle %lu: delFail=%lu stale=%lu recreFail=%lu recreMiss=%lu\n",
                  cycle, delFail, stale, recreFail, recreMiss);
        }
        ok(delFail == 0, "delete failed %lu times across cycles\n", delFail);
        ok(stale == 0, "stale $I30 entry survived delete %lu times (dangling index)\n", stale);
        ok(recreFail == 0, "re-create failed %lu times (stale key collision / dup-insert)\n", recreFail);
        ok(recreMiss == 0, "re-created name not found %lu times\n", recreMiss);
    }

    /* Phase 6: FILE_DELETE_ON_CLOSE must actually remove the file (the records
     * still exist from Phase 5's last re-create). Delete each via delete-on-
     * close, then confirm each is gone - a create that ignored the disposition
     * leaves every entry behind (dangling $I30) and a same-name create collides. */
    {
        ULONG docFail = 0, docStale = 0;
        for (i = 0; i < FILE_COUNT; i++)
        {
            RtlStringCchPrintfW(filePath, RTL_NUMBER_OF(filePath),
                                L"%ls\\curs_%05u_indexfill.3x.gz", dirPath, i);
            Status = DeleteOnCloseOne(filePath);
            if (!NT_SUCCESS(Status))
            {
                docFail++;
                if (docFail <= 5)
                    trace("delete-on-close %05u: 0x%lx\n", i, Status);
            }
        }
        for (i = 0; i < FILE_COUNT; i++)
        {
            RtlStringCchPrintfW(filePath, RTL_NUMBER_OF(filePath),
                                L"%ls\\curs_%05u_indexfill.3x.gz", dirPath, i);
            Status = CreateOne(filePath, FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL);
            if (NT_SUCCESS(Status))
            {
                docStale++;
                if (docStale <= 5)
                    trace("delete-on-close STALE: %05u still openable\n", i);
            }
        }
        trace("Phase 6: delete-on-close fail=%lu stale=%lu\n", docFail, docStale);
        ok(docFail == 0, "delete-on-close open failed %lu times\n", docFail);
        ok(docStale == 0, "FILE_DELETE_ON_CLOSE left %lu files behind (disposition ignored)\n", docStale);
    }
}

START_TEST(NtfsDirIndex)
{
    NtfsDirIndexTest();
}
