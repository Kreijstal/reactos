/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS create/open semantics
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box IRP_MJ_CREATE coverage on NTFS:
 *  - every CreateDisposition against a missing and an existing file, with
 *    the IoStatus.Information value Windows reports;
 *  - FILE_DIRECTORY_FILE / FILE_NON_DIRECTORY_FILE mismatches;
 *  - trailing backslashes, a path through a file, a missing parent;
 *  - names longer than 255 characters, full paths longer than MAX_PATH;
 *  - FILE_DELETE_ON_CLOSE, delete-pending, non-empty directory delete;
 *  - read-only attribute enforcement, sharing violations;
 *  - case-insensitive open with case-preserving storage;
 *  - open by file ID (FILE_OPEN_BY_FILE_ID with an 8-byte ID).
 *
 * Windows reference: MS-FSA 2.1.5.1, Wine ntdll/file.c test_NtCreateFile
 * and test_file_disposition_information, the NtCreateFile documentation.
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

static
NTSTATUS
CreateInfo(
    _In_ PCWSTR Path,
    _In_ ULONG Disposition,
    _In_ ULONG Options,
    _Out_ PULONG_PTR Information)
{
    HANDLE Handle;
    NTSTATUS Status;

    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE | DELETE,
                           FILE_ATTRIBUTE_NORMAL,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           Disposition, Options, NULL, 0, Information);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);
    return Status;
}

static
LONGLONG
QueryEofByPath(
    _In_ PCWSTR Path)
{
    HANDLE Handle;
    FILE_STANDARD_INFORMATION Standard;

    if (!NT_SUCCESS(NtfsKmtOpen(&Handle, Path, FILE_READ_ATTRIBUTES, 0)))
        return -1;
    NtfsKmtQueryStandard(Handle, &Standard);
    ZwClose(Handle);
    return Standard.EndOfFile.QuadPart;
}

static
VOID
TestDispositions(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    NTSTATUS Status;
    ULONG_PTR Info;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"disp.txt");

    /* Missing file. */
    Status = CreateInfo(Path, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);
    Status = CreateInfo(Path, FILE_OVERWRITE, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);

    Status = CreateInfo(Path, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, FILE_CREATED);
    Status = CreateInfo(Path, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_COLLISION);
    Status = CreateInfo(Path, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, FILE_OPENED);
    Status = CreateInfo(Path, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, FILE_OPENED);

    /* Overwrite truncates the existing data. */
    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateFileWithData(Path, "0123456789", 10);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = CreateInfo(Path, FILE_OVERWRITE, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, FILE_OVERWRITTEN);
    ok_eq_longlong(QueryEofByPath(Path), 0LL);

    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateFileWithData(Path, "0123456789", 10);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = CreateInfo(Path, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, FILE_OVERWRITTEN);
    ok_eq_longlong(QueryEofByPath(Path), 0LL);

    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateFileWithData(Path, "0123456789", 10);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = CreateInfo(Path, FILE_SUPERSEDE, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, FILE_SUPERSEDED);
    ok_eq_longlong(QueryEofByPath(Path), 0LL);

    /* The *_IF and SUPERSEDE dispositions create a missing file. */
    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = CreateInfo(Path, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, FILE_CREATED);
    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = CreateInfo(Path, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, FILE_CREATED);
    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = CreateInfo(Path, FILE_SUPERSEDE, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, FILE_CREATED);
    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

static
VOID
TestPathForms(
    _In_ PCWSTR Dir)
{
    WCHAR File[MAX_PATH];
    WCHAR SubDir[MAX_PATH];
    WCHAR Path[MAX_PATH];
    NTSTATUS Status;
    ULONG_PTR Info;

    NtfsKmtJoin(File, RTL_NUMBER_OF(File), Dir, L"plain.txt");
    NtfsKmtJoin(SubDir, RTL_NUMBER_OF(SubDir), Dir, L"subdir");
    Status = NtfsKmtCreateFileWithData(File, "x", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateDirectory(SubDir);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Type mismatches. */
    Status = CreateInfo(File, FILE_OPEN, FILE_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_NOT_A_DIRECTORY);
    Status = CreateInfo(SubDir, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_FILE_IS_A_DIRECTORY);
    Status = CreateInfo(SubDir, FILE_CREATE, FILE_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_COLLISION);

    /* A trailing backslash names a directory. */
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls\\", File);
    Status = CreateInfo(Path, FILE_OPEN, 0, &Info);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_INVALID);
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls\\", SubDir);
    Status = CreateInfo(Path, FILE_OPEN, FILE_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls\\newfile.txt\\", Dir);
    Status = CreateInfo(Path, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_INVALID);
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls\\newdir\\", Dir);
    Status = CreateInfo(Path, FILE_CREATE, FILE_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, FILE_CREATED);

    /* A file in the middle of a path, and a missing parent. */
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls\\child.txt", File);
    Status = CreateInfo(Path, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_OBJECT_PATH_NOT_FOUND);
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls\\nodir\\child.txt", Dir);
    Status = CreateInfo(Path, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_OBJECT_PATH_NOT_FOUND);

    /* Invalid characters. */
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls\\bad*name.txt", Dir);
    Status = CreateInfo(Path, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_INVALID);
}

static
VOID
TestLongNames(
    _In_ PCWSTR Dir)
{
    PWCHAR Path;
    WCHAR Component[260];
    NTSTATUS Status;
    ULONG_PTR Info;
    ULONG i, Level;
    HANDLE Handle;

    Path = NtfsKmtAlloc(NTFS_KMT_MAX_PATH * sizeof(WCHAR));
    if (Path == NULL)
        return;

    /* 255 characters is the longest component, 256 is invalid. */
    for (i = 0; i < 255; i++)
        Component[i] = (WCHAR)(L'a' + i % 26);
    Component[255] = UNICODE_NULL;
    NtfsKmtJoin(Path, NTFS_KMT_MAX_PATH, Dir, Component);
    Status = CreateInfo(Path, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = CreateInfo(Path, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Component[255] = L'z';
    Component[256] = UNICODE_NULL;
    NtfsKmtJoin(Path, NTFS_KMT_MAX_PATH, Dir, Component);
    Status = CreateInfo(Path, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &Info);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_INVALID);

    /* Five nested 100-character directories: a path far beyond MAX_PATH. */
    for (i = 0; i < 100; i++)
        Component[i] = (WCHAR)(L'A' + i % 26);
    Component[100] = UNICODE_NULL;
    RtlStringCchCopyW(Path, NTFS_KMT_MAX_PATH, Dir);
    for (Level = 0; Level < 5; Level++)
    {
        RtlStringCchCatW(Path, NTFS_KMT_MAX_PATH, L"\\");
        RtlStringCchCatW(Path, NTFS_KMT_MAX_PATH, Component);
        Status = NtfsKmtCreateDirectory(Path);
        ok(Status == STATUS_SUCCESS, "long-path mkdir level %lu: 0x%lx\n", Level, Status);
    }
    RtlStringCchCatW(Path, NTFS_KMT_MAX_PATH, L"\\deep.txt");
    ok(wcslen(Path) > MAX_PATH, "path only %Iu characters\n", wcslen(Path));
    Status = NtfsKmtCreateFileWithData(Path, "deep", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        UCHAR Data[8];
        ULONG_PTR Read;
        Status = NtfsKmtRead(Handle, 0, Data, sizeof(Data), &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Read, 4);
        ZwClose(Handle);
    }
    /* NtfsKmtDeleteTree of Dir cleans the nest. */
    NtfsKmtFree(Path);
}

static
VOID
TestDeleteSemantics(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    WCHAR SubDir[MAX_PATH];
    WCHAR Child[MAX_PATH];
    HANDLE Handle, Other;
    NTSTATUS Status;
    FILE_DISPOSITION_INFORMATION Disp;
    FILE_BASIC_INFORMATION Basic;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"doc.txt");

    /* FILE_DELETE_ON_CLOSE needs DELETE access. */
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_WRITE, 0, 0, FILE_CREATE,
                           FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);

    /* Created with FILE_DELETE_ON_CLOSE: gone after the last close. */
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_WRITE | DELETE, 0,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_CREATE,
                           FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        /* Still there while open... */
        Status = NtfsKmtOpen(&Other, Path, FILE_READ_ATTRIBUTES, 0);
        ok(Status == STATUS_SUCCESS || Status == STATUS_DELETE_PENDING,
           "open of a delete-on-close file while open: 0x%lx\n", Status);
        if (NT_SUCCESS(Status))
            ZwClose(Other);
        ZwClose(Handle);
    }
    Status = NtfsKmtOpen(&Handle, Path, FILE_READ_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);

    /* Disposition set: further opens see STATUS_DELETE_PENDING, the open
     * handle keeps working, the file is gone after the close. */
    Status = NtfsKmtCreateFileWithData(Path, "pending", 7);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ | DELETE, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        UCHAR Data[8];
        ULONG_PTR Read;

        Disp.DeleteFile = TRUE;
        Status = NtfsKmtSetInfo(Handle, &Disp, sizeof(Disp), FileDispositionInformation);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = NtfsKmtOpen(&Other, Path, FILE_READ_ATTRIBUTES, 0);
        ok_eq_hex(Status, STATUS_DELETE_PENDING);
        if (NT_SUCCESS(Status))
            ZwClose(Other);
        Status = NtfsKmtRead(Handle, 0, Data, sizeof(Data), &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Read, 7);

        /* Clearing the disposition again cancels the delete. */
        Disp.DeleteFile = FALSE;
        Status = NtfsKmtSetInfo(Handle, &Disp, sizeof(Disp), FileDispositionInformation);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Handle);
    }
    Status = NtfsKmtOpen(&Handle, Path, FILE_READ_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);
    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* A non-empty directory cannot be deleted. */
    NtfsKmtJoin(SubDir, RTL_NUMBER_OF(SubDir), Dir, L"full");
    NtfsKmtJoin(Child, RTL_NUMBER_OF(Child), SubDir, L"child.txt");
    Status = NtfsKmtCreateDirectory(SubDir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateFileWithData(Child, "c", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Handle, SubDir, DELETE, FILE_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Disp.DeleteFile = TRUE;
        Status = NtfsKmtSetInfo(Handle, &Disp, sizeof(Disp), FileDispositionInformation);
        ok_eq_hex(Status, STATUS_DIRECTORY_NOT_EMPTY);
        ZwClose(Handle);
    }
    Status = NtfsKmtDelete(Child);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtDelete(SubDir);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Read-only: no write access, no delete. */
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_WRITE, FILE_ATTRIBUTE_READONLY, 0, FILE_CREATE,
                           FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_WRITE, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);
    Status = NtfsKmtOpen(&Handle, Path, DELETE | FILE_WRITE_ATTRIBUTES, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Disp.DeleteFile = TRUE;
        Status = NtfsKmtSetInfo(Handle, &Disp, sizeof(Disp), FileDispositionInformation);
        ok_eq_hex(Status, STATUS_CANNOT_DELETE);
        RtlZeroMemory(&Basic, sizeof(Basic));
        Basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        Status = NtfsKmtSetInfo(Handle, &Basic, sizeof(Basic), FileBasicInformation);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Handle);
    }
    Status = NtfsKmtCreate(&Handle, NULL, Path, DELETE, 0, 0, FILE_OPEN,
                           FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);
}

static
VOID
TestSharingAndCase(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    WCHAR Upper[MAX_PATH];
    HANDLE Handle, Other;
    NTSTATUS Status;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"MixedCase.Txt");
    NtfsKmtJoin(Upper, RTL_NUMBER_OF(Upper), Dir, L"MIXEDCASE.TXT");
    Status = NtfsKmtCreateFileWithData(Path, "case", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* No sharing: a second open is a sharing violation. */
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ, 0, 0, FILE_OPEN,
                           FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtOpen(&Other, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SHARING_VIOLATION);
        if (NT_SUCCESS(Status))
            ZwClose(Other);
        ZwClose(Handle);
    }

    /* Case-insensitive lookup... */
    Status = NtfsKmtOpen(&Handle, Upper, FILE_READ_ATTRIBUTES, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);

    /* ...and a case-preserving directory entry. */
    Status = NtfsKmtOpen(&Handle, Dir, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        IO_STATUS_BLOCK IoStatus;
        PUCHAR Buffer = NtfsKmtAlloc(8192);
        BOOLEAN Found = FALSE, Restart = TRUE;

        while (Buffer != NULL &&
               NT_SUCCESS(ZwQueryDirectoryFile(Handle, NULL, NULL, NULL, &IoStatus, Buffer, 8192,
                                               FileNamesInformation, FALSE, NULL, Restart)))
        {
            PFILE_NAMES_INFORMATION Entry = (PFILE_NAMES_INFORMATION)Buffer;
            Restart = FALSE;
            for (;;)
            {
                UNICODE_STRING Got, Want = RTL_CONSTANT_STRING(L"MixedCase.Txt");
                NtfsKmtCounted(&Got, Entry->FileName, Entry->FileNameLength);
                if (RtlEqualUnicodeString(&Got, &Want, TRUE))
                {
                    Found = TRUE;
                    ok(RtlEqualUnicodeString(&Got, &Want, FALSE), "stored name lost its case: %wZ\n", &Got);
                }
                if (Entry->NextEntryOffset == 0)
                    break;
                Entry = (PFILE_NAMES_INFORMATION)((PUCHAR)Entry + Entry->NextEntryOffset);
            }
        }
        ok(Found, "MixedCase.Txt not in the directory listing\n");
        NtfsKmtFree(Buffer);
        ZwClose(Handle);
    }

    /* FILE_CREATE with a different case collides. */
    Status = NtfsKmtCreate(&Handle, NULL, Upper, GENERIC_READ, 0, 0, FILE_CREATE,
                           FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_COLLISION);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);
}

static
VOID
TestOpenById(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle, ById, DirHandle;
    NTSTATUS Status;
    ULONGLONG Id;
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"byid.txt");
    Status = NtfsKmtCreateFileWithData(Path, "by id", 5);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Handle, Path, FILE_READ_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Id = NtfsKmtQueryFileId(Handle);
    ZwClose(Handle);
    ok(Id != 0, "FileInternalInformation returned 0\n");

    /* Any handle on the volume serves as the root for an ID open. */
    Status = NtfsKmtOpen(&DirHandle, Dir, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Name.Buffer = (PWSTR)&Id;
    Name.Length = Name.MaximumLength = sizeof(Id);
    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_KERNEL_HANDLE, DirHandle, NULL);
    Status = ZwCreateFile(&ById, GENERIC_READ | SYNCHRONIZE, &ObjectAttributes, &IoStatus, NULL, 0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                          FILE_OPEN_BY_FILE_ID | FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        UCHAR Data[8];
        ULONG_PTR Read;

        ok(NtfsKmtQueryFileId(ById) == Id, "open by ID reached another file\n");
        Status = NtfsKmtRead(ById, 0, Data, sizeof(Data), &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(Read == 5 && RtlCompareMemory(Data, "by id", 5) == 5, "data via ID open differs\n");
        ZwClose(ById);
    }

    /* An ID that is not 8 (or 16) bytes long is invalid. */
    Name.Length = Name.MaximumLength = 6;
    Status = ZwCreateFile(&ById, GENERIC_READ | SYNCHRONIZE, &ObjectAttributes, &IoStatus, NULL, 0,
                          FILE_SHARE_READ, FILE_OPEN,
                          FILE_OPEN_BY_FILE_ID | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    if (NT_SUCCESS(Status))
        ZwClose(ById);
    ZwClose(DirHandle);
}

START_TEST(NtfsCreateSemantics)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_create", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    TestDispositions(Dir);
    TestPathForms(Dir);
    TestLongNames(Dir);
    TestDeleteSemantics(Dir);
    TestSharingAndCase(Dir);
    TestOpenById(Dir);

    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
