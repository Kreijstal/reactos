/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Deleting files while a directory is being enumerated
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "precomp.h"
#include <ndk/iofuncs.h>
#include <ndk/obfuncs.h>

/*
 * Remove-Item -Recurse, and any FindFirstFile/FindNextFile + DeleteFile loop,
 * deletes each entry it was handed before asking for the next ones. Windows
 * resumes a directory scan after the last name it returned, so the entries
 * that are still there are all returned, however much the deletions reshape
 * the directory index. Run it from a directory on the volume to test (NTFS:
 * enough files for a multi-level $I30 B+tree).
 */

#define NUM_FILES 1500
#define TEST_DIR L"DelEnumTest.tmp"

static WCHAR DirPath[MAX_PATH];
static UCHAR Seen[NUM_FILES];

static void
MakeName(PWSTR Buffer, SIZE_T Count, ULONG Index)
{
    StringCchPrintfW(Buffer, Count, L"app-%04lu-%s.json", Index,
                     (Index % 3) ? L"some-longer-manifest-name" : L"x");
}

static LONG
NameIndex(PCWSTR Name, SIZE_T Length)
{
    LONG Value = 0;
    SIZE_T i;

    if (Length < 9 || wcsncmp(Name, L"app-", 4) != 0)
        return -1;
    for (i = 4; i < 8; i++)
    {
        if (Name[i] < L'0' || Name[i] > L'9')
            return -1;
        Value = Value * 10 + (Name[i] - L'0');
    }
    return (Value < NUM_FILES) ? Value : -1;
}

static BOOL
Populate(void)
{
    WCHAR Path[MAX_PATH], Name[64];
    ULONG i;

    if (!CreateDirectoryW(DirPath, NULL))
    {
        ok(0, "CreateDirectory(%ls) failed with %lu\n", DirPath, GetLastError());
        return FALSE;
    }

    for (i = 0; i < NUM_FILES; i++)
    {
        HANDLE File;

        MakeName(Name, _countof(Name), i);
        StringCchPrintfW(Path, _countof(Path), L"%s\\%s", DirPath, Name);
        File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (File == INVALID_HANDLE_VALUE)
        {
            ok(0, "CreateFile(%ls) failed with %lu\n", Path, GetLastError());
            return FALSE;
        }
        CloseHandle(File);
    }
    return TRUE;
}

static void
DeleteRemaining(void)
{
    WCHAR Path[MAX_PATH], Name[64];
    ULONG i;

    for (i = 0; i < NUM_FILES; i++)
    {
        MakeName(Name, _countof(Name), i);
        StringCchPrintfW(Path, _countof(Path), L"%s\\%s", DirPath, Name);
        DeleteFileW(Path);
    }
    RemoveDirectoryW(DirPath);
}

static void
TestFindNextFile(void)
{
    WCHAR Pattern[MAX_PATH], Path[MAX_PATH];
    WIN32_FIND_DATAW Data;
    HANDLE Find;
    ULONG Returned = 0, Duplicates = 0, DeleteFailures = 0;
    BOOL Removed;

    if (!Populate())
        goto Cleanup;

    RtlZeroMemory(Seen, sizeof(Seen));
    StringCchPrintfW(Pattern, _countof(Pattern), L"%s\\*", DirPath);
    Find = FindFirstFileW(Pattern, &Data);
    ok(Find != INVALID_HANDLE_VALUE, "FindFirstFile failed with %lu\n", GetLastError());
    if (Find == INVALID_HANDLE_VALUE)
        goto Cleanup;

    do
    {
        LONG Index = NameIndex(Data.cFileName, wcslen(Data.cFileName));
        if (Index < 0)
            continue;
        Returned++;
        if (Seen[Index]++)
            Duplicates++;
        StringCchPrintfW(Path, _countof(Path), L"%s\\%s", DirPath, Data.cFileName);
        if (!DeleteFileW(Path))
            DeleteFailures++;
    } while (FindNextFileW(Find, &Data));

    ok_err(ERROR_NO_MORE_FILES);
    FindClose(Find);

    ok(Returned == NUM_FILES, "FindNextFile returned %lu of %u files\n", Returned, NUM_FILES);
    ok(Duplicates == 0, "%lu files returned twice\n", Duplicates);
    ok(DeleteFailures == 0, "%lu deletes failed\n", DeleteFailures);

    Removed = RemoveDirectoryW(DirPath);
    ok(Removed, "RemoveDirectory failed with %lu: the scan left files behind\n", GetLastError());
    if (Removed)
        return;

Cleanup:
    DeleteRemaining();
}

static void
TestNtQueryDirectoryFile(void)
{
    UCHAR Buffer[1024];
    WCHAR Path[MAX_PATH];
    UNICODE_STRING NtPath;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;
    HANDLE Directory;
    NTSTATUS Status;
    BOOLEAN Restart = TRUE;
    ULONG Returned = 0, Duplicates = 0, DeleteFailures = 0, Queries = 0;

    if (!Populate())
        goto Cleanup;

    RtlZeroMemory(Seen, sizeof(Seen));
    if (!RtlDosPathNameToNtPathName_U(DirPath, &NtPath, NULL, NULL))
    {
        ok(0, "RtlDosPathNameToNtPathName_U failed\n");
        goto Cleanup;
    }
    InitializeObjectAttributes(&Attributes, &NtPath, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Directory, FILE_LIST_DIRECTORY | SYNCHRONIZE, &Attributes, &Iosb,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    RtlFreeUnicodeString(&NtPath);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    for (;;)
    {
        PFILE_BOTH_DIR_INFORMATION Entry = (PFILE_BOTH_DIR_INFORMATION)Buffer;

        Status = NtQueryDirectoryFile(Directory, NULL, NULL, NULL, &Iosb, Buffer, sizeof(Buffer),
                                      FileBothDirectoryInformation, FALSE, NULL, Restart);
        if (!NT_SUCCESS(Status))
            break;
        Restart = FALSE;
        Queries++;

        for (;;)
        {
            LONG Index = NameIndex(Entry->FileName, Entry->FileNameLength / sizeof(WCHAR));
            if (Index >= 0)
            {
                Returned++;
                if (Seen[Index]++)
                    Duplicates++;

                /* By full path, as Remove-Item does it */
                StringCchPrintfW(Path, _countof(Path), L"%s\\%.*s", DirPath,
                                 (int)(Entry->FileNameLength / sizeof(WCHAR)), Entry->FileName);
                if (!DeleteFileW(Path))
                    DeleteFailures++;
            }
            if (Entry->NextEntryOffset == 0)
                break;
            Entry = (PFILE_BOTH_DIR_INFORMATION)((PUCHAR)Entry + Entry->NextEntryOffset);
        }
    }

    ok_ntstatus(Status, STATUS_NO_MORE_FILES);
    NtClose(Directory);

    ok(Returned == NUM_FILES, "NtQueryDirectoryFile returned %lu of %u files in %lu queries\n",
       Returned, NUM_FILES, Queries);
    ok(Duplicates == 0, "%lu files returned twice\n", Duplicates);
    ok(DeleteFailures == 0, "%lu deletes failed\n", DeleteFailures);

    if (RemoveDirectoryW(DirPath))
        return;
    ok(0, "RemoveDirectory failed with %lu: the scan left files behind\n", GetLastError());

Cleanup:
    DeleteRemaining();
}

START_TEST(DeleteWhileEnum)
{
    WCHAR Root[MAX_PATH], FsName[MAX_PATH];
    DWORD Length;

    Length = GetCurrentDirectoryW(_countof(DirPath), DirPath);
    ok(Length > 0 && Length < _countof(DirPath) - 32, "GetCurrentDirectory failed\n");
    if (Length == 0 || Length >= _countof(DirPath) - 32)
        return;
    if (DirPath[Length - 1] != L'\\')
        StringCchCatW(DirPath, _countof(DirPath), L"\\");
    StringCchCatW(DirPath, _countof(DirPath), TEST_DIR);

    StringCchCopyNW(Root, _countof(Root), DirPath, 3);
    if (GetVolumeInformationW(Root, NULL, 0, NULL, NULL, NULL, FsName, _countof(FsName)))
        trace("Testing in %ls on %ls\n", DirPath, FsName);

    /* Leftovers of an interrupted run */
    DeleteRemaining();

    TestFindNextFile();
    TestNtQueryDirectoryFile();
}
