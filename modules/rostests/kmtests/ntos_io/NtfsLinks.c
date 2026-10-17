/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS hard links
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box hard-link coverage through FileLinkInformation:
 *  - basic link create, shared data, link count, same file ID;
 *  - collision / replace / directory-source statuses (Wine ntdll/file.c
 *    test_file_link_information is the reference for every status);
 *  - FileHardLinkInformation enumeration (Vista+);
 *  - delete one link, the other keeps the data;
 *  - the pacman/msys2 hard-link-group scenarios that the host tests
 *    test_hardlink_groups / test_pacman_linkgroups / test_hlt_delete /
 *    test_hl2_poisoned built inside the driver: many link groups in one
 *    directory, targets carrying ~700 bytes of resident data so the very first
 *    link needs an $ATTRIBUTE_LIST, long and 8.3 names, then the rm -rf shape
 *    (enumerate once, delete every name in enumeration order).
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

static
ULONG
QueryLinkCount(
    _In_ PCWSTR Path)
{
    NTSTATUS Status;
    HANDLE Handle;
    FILE_STANDARD_INFORMATION Standard;

    Status = NtfsKmtOpen(&Handle, Path, FILE_READ_ATTRIBUTES, 0);
    if (!NT_SUCCESS(Status))
        return 0;
    Status = NtfsKmtQueryStandard(Handle, &Standard);
    ZwClose(Handle);
    return NT_SUCCESS(Status) ? Standard.NumberOfLinks : 0;
}

static
NTSTATUS
LinkPath(
    _In_ PCWSTR Existing,
    _In_ PCWSTR NewPath,
    _In_ BOOLEAN Replace)
{
    NTSTATUS Status;
    HANDLE Handle;

    /* CreateHardLinkW's access: FILE_WRITE_ATTRIBUTES on the source. */
    Status = NtfsKmtOpen(&Handle, Existing, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, 0);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtfsKmtLink(Handle, NewPath, Replace);
    ZwClose(Handle);
    return Status;
}

static
ULONGLONG
QueryIdByPath(
    _In_ PCWSTR Path)
{
    HANDLE Handle;
    ULONGLONG Id;

    if (!NT_SUCCESS(NtfsKmtOpen(&Handle, Path, FILE_READ_ATTRIBUTES, 0)))
        return 0;
    Id = NtfsKmtQueryFileId(Handle);
    ZwClose(Handle);
    return Id;
}

static
VOID
TestBasicLinks(
    _In_ PCWSTR Dir)
{
    NTSTATUS Status;
    WCHAR A[MAX_PATH], B[MAX_PATH], C[MAX_PATH], Sub[MAX_PATH], D[MAX_PATH];
    WCHAR SubDir[MAX_PATH];
    UCHAR Data[32];
    HANDLE Handle;
    ULONG_PTR Read;
    ULONGLONG IdA;
    ULONG Names = 1;            /* names the file of A has right now */
    BOOLEAN Replaced = FALSE, InSubDir = FALSE;
    UNICODE_STRING Us;

    NtfsKmtJoin(A, RTL_NUMBER_OF(A), Dir, L"a.txt");
    NtfsKmtJoin(B, RTL_NUMBER_OF(B), Dir, L"b.txt");
    NtfsKmtJoin(C, RTL_NUMBER_OF(C), Dir, L"c.txt");
    NtfsKmtJoin(SubDir, RTL_NUMBER_OF(SubDir), Dir, L"sub");
    NtfsKmtJoin(Sub, RTL_NUMBER_OF(Sub), SubDir, L"d.txt");
    NtfsKmtJoin(D, RTL_NUMBER_OF(D), Dir, L"linkdir");

    Status = NtfsKmtCreateFileWithData(A, "hello link", 10);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateFileWithData(C, "other", 5);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateDirectory(SubDir);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* oldpath is a file, newpath doesn't exist */
    Status = LinkPath(A, B, FALSE);
    if (skip(!NtfsKmtIsUnimplemented(Status), "FileLinkInformation not supported (0x%lx)\n", Status))
        return;
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        Names++;
    ok_eq_ulong(QueryLinkCount(A), 2UL);
    ok_eq_ulong(QueryLinkCount(B), 2UL);
    IdA = QueryIdByPath(A);
    ok(IdA != 0 && IdA == QueryIdByPath(B), "links have different file IDs\n");

    /* Data written through one name is visible through the other. */
    Status = NtfsKmtOpen(&Handle, B, GENERIC_WRITE, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtWrite(Handle, 0, "HELLO", 5, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Handle);
    }
    Status = NtfsKmtOpen(&Handle, A, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(Data, sizeof(Data));
        Status = NtfsKmtRead(Handle, 0, Data, sizeof(Data), &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Read, 10);
        ok(RtlCompareMemory(Data, "HELLO link", 10) == 10, "data through link mismatch: %.10s\n", Data);
        ZwClose(Handle);
    }

    /* oldpath is a file, newpath is a file, ReplaceIfExists = FALSE */
    Status = LinkPath(A, C, FALSE);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_COLLISION);
    ok_eq_ulong(QueryLinkCount(A), 2UL);

    /* oldpath == newpath */
    Status = LinkPath(A, A, FALSE);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_COLLISION);

    /* oldpath is a file, newpath is a directory, ReplaceIfExists = FALSE */
    Status = LinkPath(A, SubDir, FALSE);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_COLLISION);

    /* oldpath is a file, newpath is a file, ReplaceIfExists = TRUE: the
     * target name now refers to A's file, C's old file is gone. */
    Status = LinkPath(A, C, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Names++;
        Replaced = TRUE;
        ok_eq_ulong(QueryLinkCount(A), Names);
        ok(QueryIdByPath(C) == IdA, "replaced link does not point at the source file\n");
    }

    /* A link into another directory. */
    Status = LinkPath(A, Sub, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Names++;
        InSubDir = TRUE;
        ok_eq_ulong(QueryLinkCount(Sub), Names);
    }

    /* Directories cannot be hard-linked. */
    Status = NtfsKmtOpen(&Handle, SubDir, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                         FILE_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtLink(Handle, D, FALSE);
        ok_eq_hex(Status, STATUS_FILE_IS_A_DIRECTORY);
        ZwClose(Handle);
    }

    /* FileHardLinkInformation lists every name with its parent ID. */
    Status = NtfsKmtOpen(&Handle, A, FILE_READ_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        const ULONG Size = 2048;
        PFILE_LINKS_INFORMATION Links = NtfsKmtAlloc(Size);
        ULONGLONG DirId = QueryIdByPath(Dir);
        ULONGLONG SubId = QueryIdByPath(SubDir);

        if (Links != NULL)
        {
            Status = NtfsKmtQueryInfo(Handle, Links, Size, FileHardLinkInformation, NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
            {
                PFILE_LINK_ENTRY_INFORMATION Entry = &Links->Entry;
                ULONG Seen = 0, InDir = 0, InSub = 0, i;

                ok_eq_ulong(Links->EntriesReturned, Names);
                ok(Links->BytesNeeded <= Size, "BytesNeeded %lu\n", Links->BytesNeeded);
                for (i = 0; i < Links->EntriesReturned; i++)
                {
                    Seen++;
                    if ((ULONGLONG)Entry->ParentFileId == DirId)
                        InDir++;
                    else if ((ULONGLONG)Entry->ParentFileId == SubId)
                        InSub++;
                    else
                        trace("link entry %wZ: ParentFileId %I64x, directory IDs %I64x / %I64x\n",
                              NtfsKmtCounted(&Us, Entry->FileName, Entry->FileNameLength * sizeof(WCHAR)),
                              Entry->ParentFileId, DirId, SubId);
                    if (Entry->NextEntryOffset == 0)
                        break;
                    Entry = (PFILE_LINK_ENTRY_INFORMATION)((PUCHAR)Entry + Entry->NextEntryOffset);
                }
                ok_eq_ulong(Seen, Names);
                ok_eq_ulong(InDir, Names - (InSubDir ? 1 : 0));
                ok_eq_ulong(InSub, InSubDir ? 1UL : 0UL);
            }
            NtfsKmtFree(Links);
        }
        ZwClose(Handle);
    }

    /* Delete one name: the others keep the file and its data. */
    Status = NtfsKmtDelete(A);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Handle, A, FILE_READ_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);
    Names--;
    ok_eq_ulong(QueryLinkCount(B), Names);
    Status = NtfsKmtOpen(&Handle, B, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(Data, sizeof(Data));
        Status = NtfsKmtRead(Handle, 0, Data, sizeof(Data), &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Read, 10);
        ok(RtlCompareMemory(Data, "HELLO link", 10) == 10, "data lost after unlink\n");
        ZwClose(Handle);
    }

    /* Delete-pending on one link does not hide the other links (needs a
     * second link besides B: C when the replace worked, else the subdir one). */
    if (Replaced || InSubDir)
    {
        PCWSTR Victim = Replaced ? C : Sub;

        Status = NtfsKmtOpen(&Handle, Victim, DELETE, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            FILE_DISPOSITION_INFORMATION Disp = { TRUE };
            HANDLE Other;

            Status = NtfsKmtSetInfo(Handle, &Disp, sizeof(Disp), FileDispositionInformation);
            ok_eq_hex(Status, STATUS_SUCCESS);
            Status = NtfsKmtOpen(&Other, B, FILE_READ_ATTRIBUTES, 0);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
                ZwClose(Other);
            ZwClose(Handle);
            if (NT_SUCCESS(Status))
                Names--;
        }
        ok_eq_ulong(QueryLinkCount(B), Names);
        if (!Replaced)
            InSubDir = FALSE;   /* the subdirectory link was the victim */
    }

    if (InSubDir)
    {
        Status = NtfsKmtDelete(Sub);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
            Names--;
    }
    ok_eq_ulong(QueryLinkCount(B), 1UL);
    Status = NtfsKmtDelete(B);
    ok_eq_hex(Status, STATUS_SUCCESS);
    NtfsKmtDeleteTree(SubDir);
}

/* Enumerate Dir once (the readdir of rm -rf), return names in order. */
static
ULONG
EnumerateNames(
    _In_ PCWSTR Dir,
    _Out_writes_(MaxNames) PWCHAR *Names,
    _In_ ULONG MaxNames)
{
    NTSTATUS Status;
    HANDLE Handle;
    IO_STATUS_BLOCK IoStatus;
    PUCHAR Buffer;
    ULONG Count = 0;
    BOOLEAN Restart = TRUE;
    const ULONG BufferSize = 16384;

    Status = NtfsKmtOpen(&Handle, Dir, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status))
        return 0;
    Buffer = NtfsKmtAlloc(BufferSize);
    if (Buffer == NULL)
    {
        ZwClose(Handle);
        return 0;
    }

    for (;;)
    {
        PFILE_NAMES_INFORMATION Info;

        Status = ZwQueryDirectoryFile(Handle, NULL, NULL, NULL, &IoStatus, Buffer,
                                      BufferSize, FileNamesInformation, FALSE,
                                      NULL, Restart);
        Restart = FALSE;
        if (!NT_SUCCESS(Status))
            break;
        Info = (PFILE_NAMES_INFORMATION)Buffer;
        for (;;)
        {
            ULONG Chars = Info->FileNameLength / sizeof(WCHAR);
            BOOLEAN IsDot = (Chars == 1 && Info->FileName[0] == L'.') ||
                            (Chars == 2 && Info->FileName[0] == L'.' && Info->FileName[1] == L'.');
            if (!IsDot && Count < MaxNames)
            {
                Names[Count] = NtfsKmtAlloc((Chars + 1) * sizeof(WCHAR));
                if (Names[Count] != NULL)
                {
                    RtlCopyMemory(Names[Count], Info->FileName, Info->FileNameLength);
                    Count++;
                }
            }
            if (Info->NextEntryOffset == 0)
                break;
            Info = (PFILE_NAMES_INFORMATION)((PUCHAR)Info + Info->NextEntryOffset);
        }
    }
    ok_eq_hex(Status, STATUS_NO_MORE_FILES);

    NtfsKmtFree(Buffer);
    ZwClose(Handle);
    return Count;
}

typedef struct _LINK_GROUP
{
    PCWSTR Target;
    PCWSTR LinkFormat;
    ULONG Links;
    ULONG DataSize;
} LINK_GROUP, *PLINK_GROUP;

/* test_hardlink_groups / test_hlt_delete: two groups in one directory, enough
 * links that the base record overflows into $ATTRIBUTE_LIST children and the
 * directory index splits. test_pacman_linkgroups: targets with ~700 bytes of
 * (resident) data so the very first link must spill. */
static
VOID
TestLinkGroups(
    _In_ PCWSTR Dir)
{
    static const LINK_GROUP Groups[] =
    {
        { L"t",        L"l%lu",                       23, 0 },
        { L"tgt2",     L"LongNameAliasNumber%lu",     22, 0 },
        { L"zone_Abidjan", L"tzdata_zoneinfo_Africa_link_%lu", 12, 700 },
        { L"zone_Accra",   L"tzdata_zoneinfo_Africa_alias_%lu", 12, 700 },
        { L"zone_Bamako",  L"tzdata_posix_Africa_Bamako_%lu",   12, 700 },
    };
    NTSTATUS Status;
    WCHAR GroupDir[MAX_PATH];
    WCHAR Target[MAX_PATH];
    WCHAR Name[64];
    WCHAR Path[MAX_PATH];
    PUCHAR Data;
    PWCHAR *Names;
    ULONG g, i, Total = 0, Count;
    ULONG CreateFail = 0, CountBad = 0, OpenFail = 0, DataBad = 0, DelFail = 0;

    NtfsKmtJoin(GroupDir, RTL_NUMBER_OF(GroupDir), Dir, L"hlt");
    Status = NtfsKmtCreateDirectory(GroupDir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Data = NtfsKmtAlloc(1024);
    Names = NtfsKmtAlloc(256 * sizeof(PWCHAR));
    if (Data == NULL || Names == NULL)
    {
        NtfsKmtFree(Data);
        NtfsKmtFree(Names);
        return;
    }

    for (g = 0; g < RTL_NUMBER_OF(Groups); g++)
    {
        NtfsKmtJoin(Target, RTL_NUMBER_OF(Target), GroupDir, Groups[g].Target);
        NtfsKmtFillPattern(Data, Groups[g].DataSize, g, 0);
        Status = NtfsKmtCreateFileWithData(Target, Data, Groups[g].DataSize);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Total++;

        for (i = 1; i <= Groups[g].Links; i++)
        {
            RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), Groups[g].LinkFormat, i);
            NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), GroupDir, Name);
            Status = LinkPath(Target, Path, FALSE);
            if (!NT_SUCCESS(Status))
            {
                if (++CreateFail <= 5)
                    trace("link %ls -> %ls failed 0x%lx\n", Groups[g].Target, Name, Status);
                continue;
            }
            Total++;
            /* After every create the link count matches the names made. */
            if (QueryLinkCount(Target) != i + 1)
            {
                if (++CountBad <= 5)
                    trace("%ls: link count %lu after %lu links\n", Groups[g].Target,
                          QueryLinkCount(Target), i);
            }
        }
    }
    ok(CreateFail == 0, "%lu link creates failed\n", CreateFail);
    ok(CountBad == 0, "%lu wrong link counts after create\n", CountBad);

    /* Every name opens and shows the group's data. */
    for (g = 0; g < RTL_NUMBER_OF(Groups); g++)
    {
        for (i = 1; i <= Groups[g].Links; i++)
        {
            HANDLE Handle;
            ULONG_PTR Read;

            RtlStringCchPrintfW(Name, RTL_NUMBER_OF(Name), Groups[g].LinkFormat, i);
            NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), GroupDir, Name);
            Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
            if (!NT_SUCCESS(Status))
            {
                if (++OpenFail <= 5)
                    trace("open %ls failed 0x%lx\n", Name, Status);
                continue;
            }
            if (Groups[g].DataSize != 0)
            {
                RtlZeroMemory(Data, 1024);
                Status = NtfsKmtRead(Handle, 0, Data, 1024, &Read);
                if (!NT_SUCCESS(Status) || Read != Groups[g].DataSize ||
                    NtfsKmtCheckPattern(Data, Groups[g].DataSize, g, 0) != (SIZE_T)-1)
                {
                    if (++DataBad <= 5)
                        trace("%ls: read 0x%lx len %Iu\n", Name, Status, Read);
                }
            }
            ZwClose(Handle);
        }
    }
    ok(OpenFail == 0, "%lu link names did not open\n", OpenFail);
    ok(DataBad == 0, "%lu link names returned wrong data\n", DataBad);

    /* rm -rf: enumerate once, delete in enumeration order. */
    Count = EnumerateNames(GroupDir, Names, 256);
    ok(Count == Total, "enumerated %lu names, expected %lu\n", Count, Total);
    for (i = 0; i < Count; i++)
    {
        NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), GroupDir, Names[i]);
        Status = NtfsKmtDelete(Path);
        if (!NT_SUCCESS(Status))
        {
            if (++DelFail <= 5)
                trace("delete %ls failed 0x%lx\n", Names[i], Status);
        }
        NtfsKmtFree(Names[i]);
    }
    ok(DelFail == 0, "%lu deletes failed\n", DelFail);

    /* The directory is empty and removable. */
    Count = EnumerateNames(GroupDir, Names, 256);
    ok(Count == 0, "%lu names left after rm -rf\n", Count);
    for (i = 0; i < Count; i++)
        NtfsKmtFree(Names[i]);
    Status = NtfsKmtDelete(GroupDir);
    ok_eq_hex(Status, STATUS_SUCCESS);

    NtfsKmtFree(Names);
    NtfsKmtFree(Data);
}

/* Alternate writes through two live names, then return to a single name.
 * A clean private cache from before the link must not overwrite a later
 * write through the other name when the last alias goes away. */
static
VOID
TestLiveLinkData(
    _In_ PCWSTR Dir)
{
    WCHAR A[MAX_PATH], B[MAX_PATH];
    HANDLE First, Second;
    UCHAR Data[6];
    ULONG_PTR Read;
    NTSTATUS Status;

    NtfsKmtJoin(A, RTL_NUMBER_OF(A), Dir, L"live-cache-a");
    NtfsKmtJoin(B, RTL_NUMBER_OF(B), Dir, L"live-cache-b");
    Status = NtfsKmtCreateFileWithData(A, "abcdef", sizeof(Data));
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&First, A, GENERIC_READ | GENERIC_WRITE, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = NtfsKmtRead(First, 0, Data, sizeof(Data), &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = LinkPath(A, B, FALSE);
    if (skip(!NtfsKmtIsUnimplemented(Status), "FileLinkInformation not supported (0x%lx)\n", Status))
    {
        ZwClose(First);
        return;
    }
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Second, B, GENERIC_READ | GENERIC_WRITE, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtWrite(Second, 1, "B", 1, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Second);
    }
    Status = NtfsKmtDelete(B);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtWrite(First, 2, "C", 1, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtRead(First, 0, Data, sizeof(Data), &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Read == sizeof(Data) && RtlCompareMemory(Data, "aBCdef", sizeof(Data)) == sizeof(Data),
       "last-link transition lost the alias write\n");

    Status = LinkPath(A, B, FALSE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Second, B, GENERIC_READ | GENERIC_WRITE, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtWrite(First, 3, "D", 1, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = NtfsKmtWrite(Second, 4, "E", 1, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = NtfsKmtWrite(First, 5, "F", 1, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = NtfsKmtRead(Second, 0, Data, sizeof(Data), &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(Read == sizeof(Data) && RtlCompareMemory(Data, "aBCDEF", sizeof(Data)) == sizeof(Data),
           "alternating alias writes lost data\n");
        ZwClose(Second);
    }
    Status = NtfsKmtDelete(B);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtRead(First, 0, Data, sizeof(Data), &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Read == sizeof(Data) && RtlCompareMemory(Data, "aBCDEF", sizeof(Data)) == sizeof(Data),
       "last-link read returned a stale private cache\n");
    ZwClose(First);
}

START_TEST(NtfsLinks)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_links", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    TestBasicLinks(Dir);
    TestLiveLinkData(Dir);
    TestLinkGroups(Dir);

    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
