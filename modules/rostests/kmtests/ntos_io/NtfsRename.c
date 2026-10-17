/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     NTFS rename of individual hard links and live stream paths
 */
#include <kmt_test.h>
#include "NtfsKmt.h"

static VOID
CheckFile(PCWSTR Path, ULONGLONG Id, ULONG Links, PCSTR Expected)
{
    HANDLE File;
    FILE_STANDARD_INFORMATION Standard;
    CHAR Data[4];
    ULONG_PTR Read;
    NTSTATUS Status;

    Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    ok_eq_longlong(NtfsKmtQueryFileId(File), Id);
    Status = NtfsKmtQueryStandard(File, &Standard);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) ok_eq_ulong(Standard.NumberOfLinks, Links);
    Status = NtfsKmtRead(File, 0, Data, sizeof(Data), &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Read, sizeof(Data));
    ok(RtlEqualMemory(Data, Expected, sizeof(Data)), "rename changed the stream\n");
    ZwClose(File);
}

static VOID
CheckMissing(PCWSTR Path)
{
    HANDLE File;
    NTSTATUS Status = NtfsKmtOpen(&File, Path, FILE_READ_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);
    if (NT_SUCCESS(Status)) ZwClose(File);
}

static VOID
TestLinks(PCWSTR Dir)
{
    WCHAR Path[MAX_PATH], NewPath[MAX_PATH], Alias[MAX_PATH], Moved[MAX_PATH];
    WCHAR Child[MAX_PATH], StreamPath[MAX_PATH], Name[40];
    HANDLE File = NULL, Stream = NULL;
    ULONGLONG Id;
    NTSTATUS Status;
    ULONG i;
    UCHAR NameBuffer[sizeof(FILE_NAME_INFORMATION) + MAX_PATH * sizeof(WCHAR)];
    PFILE_NAME_INFORMATION FileName = (PFILE_NAME_INFORMATION)NameBuffer;
    CHAR StreamData[4];
    ULONG_PTR Read;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"root.bin");
    NtfsKmtJoin(NewPath, RTL_NUMBER_OF(NewPath), Dir, L"gone.bin");
    NtfsKmtJoin(Child, RTL_NUMBER_OF(Child), Dir, L"child");
    Status = NtfsKmtCreateDirectory(Child);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateFileWithData(Path, "data", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Id = NtfsKmtQueryFileId(File);
    for (i = 0; i < 25; i++)
    {
        RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"alias-%03lu.dat", i);
        NtfsKmtJoin(Alias, RTL_NUMBER_OF(Alias), Dir, Name);
        Status = NtfsKmtLink(File, Alias, FALSE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) goto Cleanup;
    }
    Status = NtfsKmtRename(File, NewPath, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    ZwClose(File); File = NULL;
    CheckMissing(Path);
    CheckFile(NewPath, Id, 26, "data");
    for (i = 0; i < 25; i++)
    {
        RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), L"alias-%03lu.dat", i);
        NtfsKmtJoin(Alias, RTL_NUMBER_OF(Alias), Dir, Name);
        CheckFile(Alias, Id, 26, "data");
    }

    /* This name is beyond the base record. Rename must update that owner,
     * retaining both the file's reference sequence and its other names. */
    NtfsKmtJoin(Moved, RTL_NUMBER_OF(Moved), Dir, L"moved-024.dat");
    Status = NtfsKmtOpen(&File, Alias, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtRename(File, Moved, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    CheckMissing(Alias);
    CheckFile(Moved, Id, 26, "data");

    RtlStringCchPrintfW(StreamPath, RTL_NUMBER_OF(StreamPath), L"%ls:keep", Moved);
    Status = NtfsKmtCreateFileWithData(StreamPath, "keep", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Stream, StreamPath, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Child, L"moved-024.dat");
    Status = NtfsKmtRename(File, Path, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    CheckMissing(Moved);
    CheckFile(Path, Id, 26, "data");
    CheckFile(NewPath, Id, 26, "data");
    Status = NtfsKmtRead(Stream, 0, StreamData, sizeof(StreamData), &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Read, sizeof(StreamData));
    ok(RtlEqualMemory(StreamData, "keep", 4), "moving a link lost its open stream\n");
    Status = NtfsKmtQueryInfo(Stream, FileName, sizeof(NameBuffer), FileNameInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        PCWSTR Suffix = L"\\child\\moved-024.dat:keep";
        ULONG SuffixBytes = (ULONG)(wcslen(Suffix) * sizeof(WCHAR));
        ok(FileName->FileNameLength >= SuffixBytes &&
           RtlEqualMemory((PUCHAR)FileName->FileName + FileName->FileNameLength - SuffixBytes,
                          Suffix, SuffixBytes), "open stream retained its old path\n");
    }
Cleanup:
    if (Stream != NULL) ZwClose(Stream);
    if (File != NULL) ZwClose(File);
}

static VOID
TestReplacement(PCWSTR Dir)
{
    WCHAR Source[MAX_PATH], Target[MAX_PATH], Alias[MAX_PATH], ReadOnly[MAX_PATH];
    HANDLE File = NULL, Other = NULL;
    ULONGLONG Id, OtherId, ReadOnlyId;
    FILE_BASIC_INFORMATION Basic;
    FILE_STANDARD_INFORMATION Standard;
    NTSTATUS Status;
    UCHAR Data[4];
    ULONG_PTR Read;

    NtfsKmtJoin(Source, RTL_NUMBER_OF(Source), Dir, L"source.bin");
    NtfsKmtJoin(Target, RTL_NUMBER_OF(Target), Dir, L"target.bin");
    NtfsKmtJoin(Alias, RTL_NUMBER_OF(Alias), Dir, L"target-alias.bin");
    NtfsKmtJoin(ReadOnly, RTL_NUMBER_OF(ReadOnly), Dir, L"readonly.bin");
    Status = NtfsKmtCreateFileWithData(Source, "data", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateFileWithData(Target, "old!", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&File, Source, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Id = NtfsKmtQueryFileId(File);
    Status = NtfsKmtOpen(&Other, Target, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    OtherId = NtfsKmtQueryFileId(Other);
    /* The helper shares DELETE; ordinary rename still cannot replace an
     * open target. POSIX replacement requires the separate Ex flag. */
    Status = NtfsKmtRename(File, Target, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    CheckFile(Source, Id, 1, "data");
    CheckFile(Target, OtherId, 1, "old!");
    Status = NtfsKmtLink(Other, Alias, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(Other); Other = NULL;
    Status = NtfsKmtOpen(&Other, Alias, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtRename(File, Target, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    ZwClose(Other); Other = NULL;
    Status = NtfsKmtRename(File, Target, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    CheckMissing(Source);
    CheckFile(Target, Id, 1, "data");
    CheckFile(Alias, OtherId, 1, "old!");

    Status = NtfsKmtCreateFileWithData(ReadOnly, "old!", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Other, ReadOnly, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    ReadOnlyId = NtfsKmtQueryFileId(Other);
    RtlZeroMemory(&Basic, sizeof(Basic));
    Basic.FileAttributes = FILE_ATTRIBUTE_READONLY;
    Status = NtfsKmtSetInfo(Other, &Basic, sizeof(Basic), FileBasicInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(Other); Other = NULL;
    Status = NtfsKmtRename(File, ReadOnly, TRUE);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    CheckFile(Target, Id, 1, "data");
    CheckFile(ReadOnly, ReadOnlyId, 1, "old!");
    ZwClose(File); File = NULL;

    NtfsKmtJoin(Source, RTL_NUMBER_OF(Source), Dir, L"same-a.bin");
    NtfsKmtJoin(Target, RTL_NUMBER_OF(Target), Dir, L"same-b.bin");
    Status = NtfsKmtCreateFileWithData(Source, "data", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&File, Source, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Id = NtfsKmtQueryFileId(File);
    Status = NtfsKmtLink(File, Target, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Other, Target, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    /* MS-FSA removes the source link; this is not a same-inode no-op. */
    Status = NtfsKmtRename(File, Target, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    CheckMissing(Source);
    CheckFile(Target, Id, 1, "data");
    Status = NtfsKmtQueryStandard(File, &Standard);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) ok_eq_ulong(Standard.NumberOfLinks, 1);
    Status = NtfsKmtRead(Other, 0, Data, sizeof(Data), &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtWrite(File, 0, "next", 4, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtRead(Other, 0, Data, sizeof(Data), &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Read, sizeof(Data));
    ok(RtlEqualMemory(Data, "next", 4), "surviving rename handles have separate cache views\n");
Cleanup:
    if (Other != NULL) ZwClose(Other);
    if (File != NULL) ZwClose(File);
}

START_TEST(NtfsRename)
{
    WCHAR Root[MAX_PATH], Dir[MAX_PATH];
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL)) return;
    Status = NtfsKmtPrepareDirectory(Root, L"KMT_NTFS_RENAME", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    TestLinks(Dir);
    TestReplacement(Dir);
    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
