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
    WCHAR Child[MAX_PATH], StreamPath[MAX_PATH], CasePath[MAX_PATH], Name[40];
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
    Status = NtfsKmtRename(File, NewPath, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    NtfsKmtJoin(CasePath, RTL_NUMBER_OF(CasePath), Dir, L"GoNe.bin");
    Status = NtfsKmtRename(File, CasePath, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtQueryInfo(File, FileName, sizeof(NameBuffer), FileNameInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        PCWSTR Suffix = L"\\GoNe.bin";
        ULONG Bytes = (ULONG)(wcslen(Suffix) * sizeof(WCHAR));
        ok(FileName->FileNameLength >= Bytes &&
           RtlEqualMemory((PUCHAR)FileName->FileName + FileName->FileNameLength - Bytes,
                          Suffix, Bytes), "case-only rename did not retain the requested spelling\n");
    }
    Status = NtfsKmtRename(File, NewPath, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
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
    NtfsKmtJoin(Alias, RTL_NUMBER_OF(Alias), Dir, L"alias-000.dat");
    NtfsKmtJoin(Moved, RTL_NUMBER_OF(Moved), Dir, L"moved-000.dat");
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
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Child, L"moved-000.dat");
    Status = NtfsKmtRename(File, Path, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    CheckMissing(Moved);
    CheckFile(Path, Id, 26, "data");
    CheckFile(NewPath, Id, 26, "data");
    /* The selected filename's extension record is crowded. A longer name
     * must be able to move to another record without losing any alias. */
    {
        WCHAR LongPath[MAX_PATH + 256], Leaf[256];
        for (i = 0; i < 255; i++) Leaf[i] = L'h';
        Leaf[255] = UNICODE_NULL;
        NtfsKmtJoin(LongPath, RTL_NUMBER_OF(LongPath), Child, Leaf);
        Status = NtfsKmtRename(File, LongPath, FALSE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            CheckMissing(Path);
            CheckFile(LongPath, Id, 26, "data");
            /* Return to the short name for the stream-path checks below. */
            Status = NtfsKmtRename(File, Path, FALSE);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
    }
    /* A denied replacement after staging a large filename must restore
     * the old name metadata, not merely leave its old index entry present. */
    {
        WCHAR Protected[MAX_PATH + 256], Leaf[256], OldAlias[MAX_PATH];
        HANDLE Target = NULL, Rename = NULL;
        FILE_BASIC_INFORMATION Basic;
        PFILE_LINKS_INFORMATION Links;
        PUCHAR LinkBuffer;
        const ULONG BufferSize = 4096;
        PFILE_LINK_ENTRY_INFORMATION Entry;
        ULONGLONG TargetId;
        BOOLEAN Found = FALSE, Ghost = FALSE;
        ULONG Offset;

        for (i = 0; i < 255; i++) Leaf[i] = L'h';
        Leaf[255] = UNICODE_NULL;
        NtfsKmtJoin(Protected, RTL_NUMBER_OF(Protected), Child, Leaf);
        NtfsKmtJoin(OldAlias, RTL_NUMBER_OF(OldAlias), Dir, L"alias-001.dat");
        Status = NtfsKmtCreateFileWithData(Protected, "lock", 4);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = NtfsKmtOpen(&Target, Protected, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            TargetId = NtfsKmtQueryFileId(Target);
            RtlZeroMemory(&Basic, sizeof(Basic));
            Basic.FileAttributes = FILE_ATTRIBUTE_READONLY;
            Status = NtfsKmtSetInfo(Target, &Basic, sizeof(Basic), FileBasicInformation);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ZwClose(Target); Target = NULL;
            Status = NtfsKmtOpen(&Rename, OldAlias, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
            {
                LinkBuffer = ExAllocatePoolWithTag(PagedPool, BufferSize, 'RtfN');
                ok(LinkBuffer != NULL, "allocating link query buffer failed\n");
                if (LinkBuffer == NULL)
                {
                    ZwClose(Rename);
                    goto Cleanup;
                }
                Links = (PFILE_LINKS_INFORMATION)LinkBuffer;
                Status = NtfsKmtRename(Rename, Protected, TRUE);
                ok_eq_hex(Status, STATUS_ACCESS_DENIED);
                CheckFile(OldAlias, Id, 26, "data");
                CheckFile(Protected, TargetId, 1, "lock");
                Status = NtfsKmtQueryInfo(Rename, Links, BufferSize, FileHardLinkInformation, NULL);
                ok_eq_hex(Status, STATUS_SUCCESS);
                if (NT_SUCCESS(Status))
                {
                    ok_eq_ulong(Links->EntriesReturned, 26);
                    Offset = FIELD_OFFSET(FILE_LINKS_INFORMATION, Entry);
                    for (i = 0; i < Links->EntriesReturned; i++)
                    {
                        if (Offset + FIELD_OFFSET(FILE_LINK_ENTRY_INFORMATION, FileName) > BufferSize)
                            break;
                        Entry = (PFILE_LINK_ENTRY_INFORMATION)(LinkBuffer + Offset);
                        if (Entry->FileNameLength > (BufferSize - Offset -
                                                     FIELD_OFFSET(FILE_LINK_ENTRY_INFORMATION, FileName)) / sizeof(WCHAR))
                            break;
                        if (Entry->FileNameLength == RTL_NUMBER_OF(L"alias-001.dat") - 1 &&
                            RtlEqualMemory(Entry->FileName, L"alias-001.dat", Entry->FileNameLength * sizeof(WCHAR)))
                            Found = TRUE;
                        if (Entry->FileNameLength == 255 && Entry->FileName[0] == L'h')
                            Ghost = TRUE;
                        if (Entry->NextEntryOffset == 0) break;
                        Offset += Entry->NextEntryOffset;
                    }
                    ok(Found, "denied rename lost the old filename attribute\n");
                    ok(!Ghost, "denied rename left a staged filename attribute\n");
                }
                ExFreePoolWithTag(LinkBuffer, 'RtfN');
                ZwClose(Rename);
            }
            Status = NtfsKmtDelete(Protected);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
    }
    Status = NtfsKmtRead(Stream, 0, StreamData, sizeof(StreamData), &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Read, sizeof(StreamData));
    ok(RtlEqualMemory(StreamData, "keep", 4), "moving a link lost its open stream\n");
    Status = NtfsKmtQueryInfo(Stream, FileName, sizeof(NameBuffer), FileNameInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        PCWSTR Suffix = L"\\child\\moved-000.dat:keep";
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

/* Replacing the last name retires every target stream, but keeps the source
 * file ID, data, and alternate streams under the destination key. */
static VOID
TestReplacementStreams(PCWSTR Dir)
{
    WCHAR Source[MAX_PATH], Target[MAX_PATH], StreamPath[MAX_PATH], WorkDir[MAX_PATH];
    HANDLE File = NULL, Other = NULL;
    FILE_ALLOCATION_INFORMATION Allocation;
    FILE_FS_SIZE_INFORMATION Before, After;
    IO_STATUS_BLOCK IoStatus;
    ULONGLONG Id;
    NTSTATUS Status;

    /* Keep this directory resident; TestReplacement above exercises the
     * allocated index of the directory containing all of the hard links. */
    NtfsKmtJoin(WorkDir, RTL_NUMBER_OF(WorkDir), Dir, L"replace-small");
    Status = NtfsKmtCreateDirectory(WorkDir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    NtfsKmtJoin(Source, RTL_NUMBER_OF(Source), WorkDir, L"replace-src.bin");
    NtfsKmtJoin(Target, RTL_NUMBER_OF(Target), WorkDir, L"replace-dst.bin");
    Status = NtfsKmtCreateFileWithData(Source, "data", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateFileWithData(Target, "old!", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    RtlStringCchPrintfW(StreamPath, RTL_NUMBER_OF(StreamPath), L"%ls:old", Target);
    Status = NtfsKmtCreateFileWithData(StreamPath, "gone", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Other, Target, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Allocation.AllocationSize.QuadPart = 1024 * 1024;
    Status = NtfsKmtSetInfo(Other, &Allocation, sizeof(Allocation), FileAllocationInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ZwQueryVolumeInformationFile(Other, &IoStatus, &Before, sizeof(Before), FileFsSizeInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(Other); Other = NULL;
    RtlStringCchPrintfW(StreamPath, RTL_NUMBER_OF(StreamPath), L"%ls:keep", Source);
    Status = NtfsKmtCreateFileWithData(StreamPath, "keep", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&File, Source, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Id = NtfsKmtQueryFileId(File);
    Status = NtfsKmtRename(File, Target, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        CheckMissing(Source);
        CheckFile(Target, Id, 1, "data");
        RtlStringCchPrintfW(StreamPath, RTL_NUMBER_OF(StreamPath), L"%ls:old", Target);
        CheckMissing(StreamPath);
        RtlStringCchPrintfW(StreamPath, RTL_NUMBER_OF(StreamPath), L"%ls:keep", Target);
        CheckFile(StreamPath, Id, 1, "keep");
        Status = ZwQueryVolumeInformationFile(File, &IoStatus, &After, sizeof(After), FileFsSizeInformation);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
            ok(After.AvailableAllocationUnits.QuadPart > Before.AvailableAllocationUnits.QuadPart,
               "last-link replacement did not reclaim target allocation\n");
    }
    ZwClose(File);
}

static VOID
TestLongName(PCWSTR Dir)
{
    WCHAR Source[MAX_PATH], Target[MAX_PATH + 256], Leaf[256];
    HANDLE File = NULL;
    UCHAR Data[600], ReadBack[600];
    ULONGLONG Id;
    ULONG_PTR Read;
    ULONG i;
    NTSTATUS Status;

    NtfsKmtJoin(Source, RTL_NUMBER_OF(Source), Dir, L"grow.bin");
    for (i = 0; i < 255; i++) Leaf[i] = L'g';
    Leaf[255] = UNICODE_NULL;
    NtfsKmtJoin(Target, RTL_NUMBER_OF(Target), Dir, Leaf);
    for (i = 0; i < sizeof(Data); i++) Data[i] = (UCHAR)(i * 13 + 7);
    Status = NtfsKmtCreateFileWithData(Source, Data, sizeof(Data));
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&File, Source, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Id = NtfsKmtQueryFileId(File);
    Status = NtfsKmtRename(File, Target, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    CheckMissing(Source);
    ZwClose(File); File = NULL;
    Status = NtfsKmtOpen(&File, Target, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    ok_eq_longlong(NtfsKmtQueryFileId(File), Id);
    Status = NtfsKmtRead(File, 0, ReadBack, sizeof(ReadBack), &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Read, sizeof(ReadBack));
    ok(RtlEqualMemory(Data, ReadBack, sizeof(Data)), "making room for a long name changed data\n");
Cleanup:
    if (File != NULL) ZwClose(File);
}

static VOID
TestRenameTimes(PCWSTR Dir)
{
    WCHAR Source[MAX_PATH], Target[MAX_PATH];
    HANDLE File = NULL;
    FILE_BASIC_INFORMATION Basic, Before, After;
    NTSTATUS Status;
    const LONGLONG OldTime = 130000000000000000LL;

    NtfsKmtJoin(Source, RTL_NUMBER_OF(Source), Dir, L"time-src.bin");
    NtfsKmtJoin(Target, RTL_NUMBER_OF(Target), Dir, L"time-dst.bin");
    Status = NtfsKmtCreateFileWithData(Source, "time", 4);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&File, Source, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    RtlZeroMemory(&Basic, sizeof(Basic));
    Basic.ChangeTime.QuadPart = OldTime;
    Basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    Status = NtfsKmtSetInfo(File, &Basic, sizeof(Basic), FileBasicInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(File); File = NULL;
    Status = NtfsKmtOpen(&File, Source, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = NtfsKmtQueryInfo(File, &Before, sizeof(Before), FileBasicInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtRename(File, Target, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtQueryInfo(File, &After, sizeof(After), FileBasicInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_longlong(After.ChangeTime.QuadPart, OldTime);
        ok(After.FileAttributes & FILE_ATTRIBUTE_ARCHIVE, "rename did not set archive\n");
    }
    ZwClose(File); File = NULL;
    Status = NtfsKmtOpen(&File, Target, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = NtfsKmtQueryInfo(File, &After, sizeof(After), FileBasicInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(After.ChangeTime.QuadPart > OldTime, "cleanup did not stamp rename change time\n");
        ok_eq_longlong(After.LastWriteTime.QuadPart, Before.LastWriteTime.QuadPart);
    }
    /* A rename to precisely the same name changes no timestamp. */
    Before = After;
    Status = NtfsKmtRename(File, Target, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(File); File = NULL;
    Status = NtfsKmtOpen(&File, Target, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = NtfsKmtQueryInfo(File, &After, sizeof(After), FileBasicInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) ok_eq_longlong(After.ChangeTime.QuadPart, Before.ChangeTime.QuadPart);
    /* Explicit per-open timestamp policy survives the rename. */
    RtlZeroMemory(&Basic, sizeof(Basic));
    Basic.ChangeTime.QuadPart = OldTime;
    Status = NtfsKmtSetInfo(File, &Basic, sizeof(Basic), FileBasicInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtRename(File, Source, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(File); File = NULL;
    Status = NtfsKmtOpen(&File, Source, FILE_READ_ATTRIBUTES, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = NtfsKmtQueryInfo(File, &After, sizeof(After), FileBasicInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) ok_eq_longlong(After.ChangeTime.QuadPart, OldTime);
    ZwClose(File);
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
    TestReplacementStreams(Dir);
    TestLongName(Dir);
    TestRenameTimes(Dir);
    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
