/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS information classes
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box coverage of the file, directory and volume information classes
 * an NTFS volume answers: FileAllInformation, FileNameInformation (and its
 * STATUS_BUFFER_OVERFLOW contract), FileAlternateNameInformation,
 * FileIdBothDirectoryInformation / FileIdFullDirectoryInformation and
 * directory-query masks, FileFsFullSizeInformation and the
 * FileFsAttributeInformation flags of NTFS.
 *
 * Replaces the 8.3 lookup host test test_appdata_lookup ("Application Data"
 * vs APPLIC~1) with a black-box short-name lookup.
 *
 * Windows reference: MS-FSCC 2.4 / 2.5, Wine ntdll/file.c
 * test_file_name_information / test_file_all_information.
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

#define DIR_BUFFER_SIZE     8192

static
VOID
TestFileInfo(
    _In_ PCWSTR Dir,
    _In_ PCWSTR DirName)
{
    WCHAR Path[MAX_PATH];
    WCHAR Expected[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    PFILE_ALL_INFORMATION All;
    ULONG_PTR Info;
    ULONG AllSize = sizeof(FILE_ALL_INFORMATION) + MAX_PATH * sizeof(WCHAR);
    UCHAR NameBuffer[sizeof(FILE_NAME_INFORMATION) + 16];
    PFILE_NAME_INFORMATION Name = (PFILE_NAME_INFORMATION)NameBuffer;
    ULONG ExpectedLength;
    UNICODE_STRING Us;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"info.txt");
    Status = NtfsKmtCreateFileWithData(Path, "0123456789", 10);
    ok_eq_hex(Status, STATUS_SUCCESS);
    RtlStringCchPrintfW(Expected, RTL_NUMBER_OF(Expected), L"\\%ls\\info.txt", DirName);
    ExpectedLength = (ULONG)(wcslen(Expected) * sizeof(WCHAR));

    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    All = NtfsKmtAlloc(AllSize);
    if (All != NULL)
    {
        Status = NtfsKmtQueryInfo(Handle, All, AllSize, FileAllInformation, &Info);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok(All->BasicInformation.FileAttributes & FILE_ATTRIBUTE_ARCHIVE,
               "new file lacks FILE_ATTRIBUTE_ARCHIVE: 0x%lx\n", All->BasicInformation.FileAttributes);
            ok(All->BasicInformation.CreationTime.QuadPart != 0, "CreationTime is 0\n");
            ok_eq_longlong(All->StandardInformation.EndOfFile.QuadPart, 10LL);
            ok_eq_ulong(All->StandardInformation.NumberOfLinks, 1UL);
            ok_eq_bool(All->StandardInformation.Directory, FALSE);
            ok_eq_bool(All->StandardInformation.DeletePending, FALSE);
            ok(All->InternalInformation.IndexNumber.QuadPart == (LONGLONG)NtfsKmtQueryFileId(Handle),
               "FileAllInformation IndexNumber differs from FileInternalInformation\n");
            ok_eq_ulong(All->NameInformation.FileNameLength, ExpectedLength);
            ok(RtlCompareMemory(All->NameInformation.FileName, Expected, ExpectedLength) == ExpectedLength,
               "FileAllInformation name %wZ, expected %ls\n",
               NtfsKmtCounted(&Us, All->NameInformation.FileName, All->NameInformation.FileNameLength), Expected);
            ok_eq_ulongptr(Info, FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName) + ExpectedLength);
        }

        /* Room for the fixed part only: overflow, the name is truncated. */
        RtlZeroMemory(All, AllSize);
        Status = NtfsKmtQueryInfo(Handle, All, sizeof(FILE_ALL_INFORMATION), FileAllInformation, &Info);
        ok_eq_hex(Status, STATUS_BUFFER_OVERFLOW);
        ok_eq_ulongptr(Info, sizeof(FILE_ALL_INFORMATION));
        ok_eq_ulong(All->NameInformation.FileNameLength, ExpectedLength);
        ok_eq_longlong(All->StandardInformation.EndOfFile.QuadPart, 10LL);
        NtfsKmtFree(All);
    }

    /* FileNameInformation into sizeof(FILE_NAME_INFORMATION): overflow, two
     * characters copied, full length reported (Wine test_file_name_information). */
    RtlFillMemory(NameBuffer, sizeof(NameBuffer), 0xCC);
    Status = NtfsKmtQueryInfo(Handle, Name, sizeof(FILE_NAME_INFORMATION), FileNameInformation, &Info);
    ok_eq_hex(Status, STATUS_BUFFER_OVERFLOW);
    ok_eq_ulongptr(Info, sizeof(FILE_NAME_INFORMATION));
    ok_eq_ulong(Name->FileNameLength, ExpectedLength);
    ok(Name->FileName[0] == L'\\', "first name character 0x%x\n", Name->FileName[0]);
    ok(Name->FileName[2] == 0xCCCC, "wrote past the buffer: 0x%x\n", Name->FileName[2]);

    /* A buffer below the fixed size is a length mismatch. */
    Status = NtfsKmtQueryInfo(Handle, Name, sizeof(FILE_NAME_INFORMATION) - 1, FileNameInformation, NULL);
    ok_eq_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    ZwClose(Handle);
}

/* Query one entry of Dir by exact name with the given class. */
static
NTSTATUS
QueryOne(
    _In_ PCWSTR Dir,
    _In_ PCWSTR Mask,
    _In_ FILE_INFORMATION_CLASS Class,
    _Out_writes_bytes_(DIR_BUFFER_SIZE) PVOID Buffer)
{
    HANDLE Handle;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    UNICODE_STRING Name;

    RtlInitUnicodeString(&Name, Mask);
    Status = NtfsKmtOpen(&Handle, Dir, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status))
        return Status;
    RtlZeroMemory(Buffer, DIR_BUFFER_SIZE);
    Status = ZwQueryDirectoryFile(Handle, NULL, NULL, NULL, &IoStatus, Buffer, DIR_BUFFER_SIZE,
                                  Class, TRUE, &Name, TRUE);
    ZwClose(Handle);
    return Status;
}

static
VOID
TestShortNamesAndDirectoryClasses(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    UCHAR AltBuffer[sizeof(FILE_NAME_INFORMATION) + 32 * sizeof(WCHAR)];
    PFILE_NAME_INFORMATION Alt = (PFILE_NAME_INFORMATION)AltBuffer;
    PUCHAR Buffer;
    ULONGLONG Id = 0;
    BOOLEAN HaveShort = FALSE;
    WCHAR Short[13];
    ULONG ShortLength = 0;
    UNICODE_STRING Us;

    Buffer = NtfsKmtAlloc(DIR_BUFFER_SIZE);
    if (Buffer == NULL)
        return;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"Long File Name For Short.txt");
    Status = NtfsKmtCreateFileWithData(Path, "longname", 8);
    ok_eq_hex(Status, STATUS_SUCCESS);
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"Application Data");
    Status = NtfsKmtCreateDirectory(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"other.dat");
    Status = NtfsKmtCreateFileWithData(Path, "o", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* FileAlternateNameInformation: either no 8.3 name (generation off) or
     * the Windows "first six~1" form; whatever it is, lookups by it work. */
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"Long File Name For Short.txt");
    Status = NtfsKmtOpen(&Handle, Path, FILE_READ_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Id = NtfsKmtQueryFileId(Handle);
        RtlZeroMemory(AltBuffer, sizeof(AltBuffer));
        Status = NtfsKmtQueryInfo(Handle, Alt, sizeof(AltBuffer), FileAlternateNameInformation, NULL);
        ok(Status == STATUS_SUCCESS || Status == STATUS_OBJECT_NAME_NOT_FOUND,
           "FileAlternateNameInformation: 0x%lx\n", Status);
        if (Status == STATUS_SUCCESS)
        {
            HaveShort = TRUE;
            ShortLength = Alt->FileNameLength;
            ok(ShortLength == 12 * sizeof(WCHAR) &&
               RtlCompareMemory(Alt->FileName, L"LONGFI~1.TXT", 12 * sizeof(WCHAR)) == 12 * sizeof(WCHAR),
               "short name %wZ, expected LONGFI~1.TXT\n", NtfsKmtCounted(&Us, Alt->FileName, ShortLength));
            RtlZeroMemory(Short, sizeof(Short));
            RtlCopyMemory(Short, Alt->FileName, min(ShortLength, 12 * sizeof(WCHAR)));
        }
        ZwClose(Handle);
    }
    if (HaveShort)
    {
        HANDLE ByShort;
        NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, Short);
        Status = NtfsKmtOpen(&ByShort, Path, FILE_READ_ATTRIBUTES, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok(NtfsKmtQueryFileId(ByShort) == Id, "short-name open reached another file\n");
            ZwClose(ByShort);
        }
        /* test_appdata_lookup: the long name keeps working next to the 8.3
         * alias ("Application Data" vs APPLIC~1). */
        NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"APPLIC~1");
        Status = NtfsKmtOpen(&ByShort, Path, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
            ZwClose(ByShort);
    }
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"Application Data");
    Status = NtfsKmtOpen(&Handle, Path, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);

    /* FileIdBothDirectoryInformation: ID, size, short name. */
    Status = QueryOne(Dir, L"Long File Name For Short.txt", FileIdBothDirectoryInformation, Buffer);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        PFILE_ID_BOTH_DIR_INFORMATION Both = (PFILE_ID_BOTH_DIR_INFORMATION)Buffer;
        ok_eq_ulong(Both->NextEntryOffset, 0UL);
        ok(Both->FileId.QuadPart == (LONGLONG)Id, "FileId %I64x, expected %I64x\n", Both->FileId.QuadPart, Id);
        ok_eq_longlong(Both->EndOfFile.QuadPart, 8LL);
        ok(Both->AllocationSize.QuadPart >= 8, "AllocationSize %I64d\n", Both->AllocationSize.QuadPart);
        ok(Both->FileNameLength == 28 * sizeof(WCHAR) &&
           RtlCompareMemory(Both->FileName, L"Long File Name For Short.txt", 28 * sizeof(WCHAR)) == 28 * sizeof(WCHAR),
           "entry name mismatch\n");
        ok(!(Both->FileAttributes & FILE_ATTRIBUTE_DIRECTORY), "file reported as directory\n");
        if (HaveShort)
        {
            ok_eq_uint(Both->ShortNameLength, (USHORT)ShortLength);
            ok(RtlCompareMemory(Both->ShortName, Short, ShortLength) == ShortLength,
               "ShortName differs from FileAlternateNameInformation\n");
        }
        else
        {
            ok_eq_uint(Both->ShortNameLength, 0);
        }
    }

    /* FileIdFullDirectoryInformation of a directory entry. */
    Status = QueryOne(Dir, L"Application Data", FileIdFullDirectoryInformation, Buffer);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        PFILE_ID_FULL_DIR_INFORMATION Full = (PFILE_ID_FULL_DIR_INFORMATION)Buffer;
        ok(Full->FileAttributes & FILE_ATTRIBUTE_DIRECTORY, "directory entry lacks FILE_ATTRIBUTE_DIRECTORY\n");
        ok(Full->FileId.QuadPart != 0, "directory FileId is 0\n");
        ok_eq_ulong(Full->EaSize, 0UL);
    }

    /* Wildcards: "*.dat" matches only other.dat; no match at all is
     * STATUS_NO_SUCH_FILE on the first query. */
    Status = QueryOne(Dir, L"*.dat", FileNamesInformation, Buffer);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        PFILE_NAMES_INFORMATION Names = (PFILE_NAMES_INFORMATION)Buffer;
        ok(Names->FileNameLength == 9 * sizeof(WCHAR) &&
           RtlCompareMemory(Names->FileName, L"other.dat", 9 * sizeof(WCHAR)) == 9 * sizeof(WCHAR),
           "*.dat matched %wZ\n", NtfsKmtCounted(&Us, Names->FileName, Names->FileNameLength));
    }
    Status = QueryOne(Dir, L"nomatch*.xyz", FileNamesInformation, Buffer);
    ok_eq_hex(Status, STATUS_NO_SUCH_FILE);

    /* A full listing of a subdirectory starts with "." and "..". */
    {
        HANDLE DirHandle;
        IO_STATUS_BLOCK IoStatus;
        NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"Application Data");
        Status = NtfsKmtOpen(&DirHandle, Path, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            Status = ZwQueryDirectoryFile(DirHandle, NULL, NULL, NULL, &IoStatus, Buffer, DIR_BUFFER_SIZE,
                                          FileBothDirectoryInformation, FALSE, NULL, TRUE);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
            {
                PFILE_BOTH_DIR_INFORMATION E = (PFILE_BOTH_DIR_INFORMATION)Buffer;
                ok(E->FileNameLength == sizeof(WCHAR) && E->FileName[0] == L'.', "first entry is not \".\"\n");
                ok(E->NextEntryOffset != 0, "only one entry in an empty directory listing\n");
                if (E->NextEntryOffset != 0)
                {
                    E = (PFILE_BOTH_DIR_INFORMATION)((PUCHAR)E + E->NextEntryOffset);
                    ok(E->FileNameLength == 2 * sizeof(WCHAR) && E->FileName[0] == L'.' && E->FileName[1] == L'.',
                       "second entry is not \"..\"\n");
                    ok_eq_ulong(E->NextEntryOffset, 0UL);
                }
            }
            Status = ZwQueryDirectoryFile(DirHandle, NULL, NULL, NULL, &IoStatus, Buffer, DIR_BUFFER_SIZE,
                                          FileBothDirectoryInformation, FALSE, NULL, FALSE);
            ok_eq_hex(Status, STATUS_NO_MORE_FILES);
            ZwClose(DirHandle);
        }
    }
    NtfsKmtFree(Buffer);
}

/* FileInternalInformation of a directory is its file reference: stable
 * across opens and equal to the FileId its parent's listing reports. */
static
VOID
TestDirectoryIds(
    _In_ PCWSTR Root,
    _In_ PCWSTR Dir,
    _In_ PCWSTR DirName)
{
    WCHAR Sub[MAX_PATH];
    PCWSTR Parents[2];
    PCWSTR Paths[2];
    PCWSTR Names[2];
    PUCHAR Buffer;
    ULONG i;
    NTSTATUS Status;

    NtfsKmtJoin(Sub, RTL_NUMBER_OF(Sub), Dir, L"iddir");
    Status = NtfsKmtCreateDirectory(Sub);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Parents[0] = Root; Paths[0] = Dir; Names[0] = DirName;
    Parents[1] = Dir;  Paths[1] = Sub; Names[1] = L"iddir";

    Buffer = NtfsKmtAlloc(DIR_BUFFER_SIZE);
    if (Buffer == NULL)
        return;
    for (i = 0; i < 2; i++)
    {
        HANDLE Handle;
        ULONGLONG Id1 = 0, Id2 = 0;

        if (NT_SUCCESS(NtfsKmtOpen(&Handle, Paths[i], FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE)))
        {
            Id1 = NtfsKmtQueryFileId(Handle);
            ZwClose(Handle);
        }
        if (NT_SUCCESS(NtfsKmtOpen(&Handle, Paths[i], FILE_READ_ATTRIBUTES, FILE_DIRECTORY_FILE)))
        {
            Id2 = NtfsKmtQueryFileId(Handle);
            ZwClose(Handle);
        }
        ok(Id1 != 0 && Id1 == Id2, "%ls: directory ID not stable across opens (%I64x / %I64x)\n",
           Names[i], Id1, Id2);
        /* Opened without FILE_DIRECTORY_FILE it is still the same directory. */
        Id2 = 0;
        if (NT_SUCCESS(NtfsKmtOpen(&Handle, Paths[i], FILE_READ_ATTRIBUTES, 0)))
        {
            Id2 = NtfsKmtQueryFileId(Handle);
            ZwClose(Handle);
        }
        ok(Id1 == Id2, "%ls: directory ID %I64x when opened without FILE_DIRECTORY_FILE, %I64x with it\n",
           Names[i], Id2, Id1);
        Status = QueryOne(Parents[i], Names[i], FileIdBothDirectoryInformation, Buffer);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
            ok(((PFILE_ID_BOTH_DIR_INFORMATION)Buffer)->FileId.QuadPart == (LONGLONG)Id1,
               "%ls: listing FileId %I64x, FileInternalInformation %I64x\n", Names[i],
               ((PFILE_ID_BOTH_DIR_INFORMATION)Buffer)->FileId.QuadPart, Id1);
    }
    NtfsKmtFree(Buffer);
}

static
VOID
TestVolumeInfo(
    _In_ PCWSTR Dir)
{
    HANDLE Handle;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    FILE_FS_SIZE_INFORMATION Size;
    FILE_FS_FULL_SIZE_INFORMATION Full;
    UCHAR AttrBuffer[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 32 * sizeof(WCHAR)];
    PFILE_FS_ATTRIBUTE_INFORMATION Attr = (PFILE_FS_ATTRIBUTE_INFORMATION)AttrBuffer;
    static const struct { ULONG Flag; PCSTR Name; } NtfsFlags[] =
    {
        { FILE_CASE_SENSITIVE_SEARCH,        "FILE_CASE_SENSITIVE_SEARCH" },
        { FILE_CASE_PRESERVED_NAMES,         "FILE_CASE_PRESERVED_NAMES" },
        { FILE_UNICODE_ON_DISK,              "FILE_UNICODE_ON_DISK" },
        { FILE_PERSISTENT_ACLS,              "FILE_PERSISTENT_ACLS" },
        { FILE_FILE_COMPRESSION,             "FILE_FILE_COMPRESSION" },
        { FILE_VOLUME_QUOTAS,                "FILE_VOLUME_QUOTAS" },
        { FILE_SUPPORTS_SPARSE_FILES,        "FILE_SUPPORTS_SPARSE_FILES" },
        { FILE_SUPPORTS_REPARSE_POINTS,      "FILE_SUPPORTS_REPARSE_POINTS" },
        { FILE_SUPPORTS_OBJECT_IDS,          "FILE_SUPPORTS_OBJECT_IDS" },
        { FILE_NAMED_STREAMS,                "FILE_NAMED_STREAMS" },
        { FILE_SUPPORTS_HARD_LINKS,          "FILE_SUPPORTS_HARD_LINKS" },
        { FILE_SUPPORTS_EXTENDED_ATTRIBUTES, "FILE_SUPPORTS_EXTENDED_ATTRIBUTES" },
        { FILE_SUPPORTS_OPEN_BY_FILE_ID,     "FILE_SUPPORTS_OPEN_BY_FILE_ID" },
        { FILE_SUPPORTS_USN_JOURNAL,         "FILE_SUPPORTS_USN_JOURNAL" },
    };
    ULONG i;

    Status = NtfsKmtOpen(&Handle, Dir, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = ZwQueryVolumeInformationFile(Handle, &IoStatus, &Size, sizeof(Size), FileFsSizeInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = ZwQueryVolumeInformationFile(Handle, &IoStatus, &Full, sizeof(Full), FileFsFullSizeInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(IoStatus.Information, sizeof(FILE_FS_FULL_SIZE_INFORMATION));
    if (NT_SUCCESS(Status))
    {
        ok_eq_longlong(Full.TotalAllocationUnits.QuadPart, Size.TotalAllocationUnits.QuadPart);
        ok_eq_ulong(Full.SectorsPerAllocationUnit, Size.SectorsPerAllocationUnit);
        ok_eq_ulong(Full.BytesPerSector, Size.BytesPerSector);
        ok(Full.CallerAvailableAllocationUnits.QuadPart <= Full.ActualAvailableAllocationUnits.QuadPart,
           "caller-available %I64d > actual %I64d\n",
           Full.CallerAvailableAllocationUnits.QuadPart, Full.ActualAvailableAllocationUnits.QuadPart);
        ok(Full.ActualAvailableAllocationUnits.QuadPart <= Full.TotalAllocationUnits.QuadPart,
           "available %I64d > total %I64d\n",
           Full.ActualAvailableAllocationUnits.QuadPart, Full.TotalAllocationUnits.QuadPart);
    }

    RtlZeroMemory(AttrBuffer, sizeof(AttrBuffer));
    Status = ZwQueryVolumeInformationFile(Handle, &IoStatus, Attr, sizeof(AttrBuffer), FileFsAttributeInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_long(Attr->MaximumComponentNameLength, 255L);
        ok_eq_ulong(Attr->FileSystemNameLength, 8UL);
        for (i = 0; i < RTL_NUMBER_OF(NtfsFlags); i++)
            ok(Attr->FileSystemAttributes & NtfsFlags[i].Flag, "%s not reported (attributes 0x%lx)\n",
               NtfsFlags[i].Name, Attr->FileSystemAttributes);
        ok(!(Attr->FileSystemAttributes & FILE_READ_ONLY_VOLUME), "writable volume reported read-only\n");
    }

    /* Too small for the name: overflow with the fixed part filled. */
    RtlZeroMemory(AttrBuffer, sizeof(AttrBuffer));
    Status = ZwQueryVolumeInformationFile(Handle, &IoStatus, Attr, sizeof(FILE_FS_ATTRIBUTE_INFORMATION),
                                          FileFsAttributeInformation);
    ok_eq_hex(Status, STATUS_BUFFER_OVERFLOW);
    ok_eq_long(Attr->MaximumComponentNameLength, 255L);
    ZwClose(Handle);
}

START_TEST(NtfsInfoClasses)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_info", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    TestFileInfo(Dir, L"kmt_ntfs_info");
    TestShortNamesAndDirectoryClasses(Dir);
    TestDirectoryIds(Root, Dir, L"kmt_ntfs_info");
    TestVolumeInfo(Dir);

    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
