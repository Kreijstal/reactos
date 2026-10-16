/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS alternate data streams
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box ADS coverage through the NT API: create/open "file:stream",
 * per-stream data independence, resident and non-resident named streams,
 * FileStreamInformation enumeration, stream delete, and the fate of the
 * named streams when the base file is deleted.
 *
 * Windows reference: MS-FSA 2.1.5.1 (stream name parsing), MS-FSCC 2.4.43
 * (FILE_STREAM_INFORMATION), Wine ntdll/file.c.
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

#define BIG_STREAM_SIZE     (3 * 4096 + 123)

typedef struct _STREAM_EXPECT
{
    PCWSTR Name;            /* as reported by FileStreamInformation */
    ULONGLONG Size;
    BOOLEAN Seen;
} STREAM_EXPECT, *PSTREAM_EXPECT;

static
NTSTATUS
WriteNew(
    _In_ PCWSTR Path,
    _In_ const VOID *Data,
    _In_ ULONG Length,
    _Out_opt_ PULONG_PTR Information)
{
    NTSTATUS Status;
    HANDLE Handle;

    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE,
                           FILE_ATTRIBUTE_NORMAL,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0,
                           Information);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtfsKmtWrite(Handle, 0, Data, Length, NULL);
    ZwClose(Handle);
    return Status;
}

/* Open Path, read up to Length bytes from offset 0, report the byte count. */
static
NTSTATUS
ReadAll(
    _In_ PCWSTR Path,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG_PTR Read)
{
    NTSTATUS Status;
    HANDLE Handle;

    *Read = 0;
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtfsKmtRead(Handle, 0, Buffer, Length, Read);
    ZwClose(Handle);
    return Status;
}

static
VOID
CheckStreamList(
    _In_ PCWSTR BasePath,
    _Inout_updates_(Count) PSTREAM_EXPECT Expect,
    _In_ ULONG Count)
{
    NTSTATUS Status;
    HANDLE Handle;
    PUCHAR Buffer;
    PFILE_STREAM_INFORMATION Info;
    ULONG Entries = 0;
    ULONG i;
    const ULONG BufferSize = 4096;

    for (i = 0; i < Count; i++)
        Expect[i].Seen = FALSE;

    Status = NtfsKmtOpen(&Handle, BasePath, FILE_READ_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Buffer = NtfsKmtAlloc(BufferSize);
    if (Buffer == NULL)
    {
        ZwClose(Handle);
        return;
    }

    Status = NtfsKmtQueryInfo(Handle, Buffer, BufferSize, FileStreamInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Info = (PFILE_STREAM_INFORMATION)Buffer;
        for (;;)
        {
            UNICODE_STRING Got;
            BOOLEAN Matched = FALSE;

            Got.Buffer = Info->StreamName;
            Got.Length = Got.MaximumLength = (USHORT)Info->StreamNameLength;
            Entries++;

            for (i = 0; i < Count; i++)
            {
                UNICODE_STRING Want;
                RtlInitUnicodeString(&Want, Expect[i].Name);
                if (RtlEqualUnicodeString(&Got, &Want, TRUE))
                {
                    ok(!Expect[i].Seen, "stream %ls reported twice\n", Expect[i].Name);
                    Expect[i].Seen = TRUE;
                    Matched = TRUE;
                    ok(Info->StreamSize.QuadPart == (LONGLONG)Expect[i].Size,
                       "stream %ls size %I64d, expected %I64u\n",
                       Expect[i].Name, Info->StreamSize.QuadPart, Expect[i].Size);
                    ok(Info->StreamAllocationSize.QuadPart >= Info->StreamSize.QuadPart,
                       "stream %ls allocation %I64d < size %I64d\n", Expect[i].Name,
                       Info->StreamAllocationSize.QuadPart, Info->StreamSize.QuadPart);
                }
            }
            ok(Matched, "unexpected stream '%wZ'\n", &Got);

            if (Info->NextEntryOffset == 0)
                break;
            ok((Info->NextEntryOffset % sizeof(LONGLONG)) == 0,
               "NextEntryOffset %lu not 8-byte aligned\n", Info->NextEntryOffset);
            Info = (PFILE_STREAM_INFORMATION)((PUCHAR)Info + Info->NextEntryOffset);
        }

        /* The unnamed stream is the first attribute of the record. */
        Info = (PFILE_STREAM_INFORMATION)Buffer;
        ok(Info->StreamNameLength == 7 * sizeof(WCHAR) &&
           RtlCompareMemory(Info->StreamName, L"::$DATA", 7 * sizeof(WCHAR)) == 7 * sizeof(WCHAR),
           "first stream is not ::$DATA\n");
    }

    ok(Entries == Count, "FileStreamInformation returned %lu entries, expected %lu\n",
       Entries, Count);
    for (i = 0; i < Count; i++)
        ok(Expect[i].Seen, "stream %ls not reported\n", Expect[i].Name);

    /* A buffer that holds exactly the "::$DATA" entry: with more streams
     * behind it the query reports STATUS_BUFFER_OVERFLOW, otherwise it is
     * complete. */
    RtlZeroMemory(Buffer, BufferSize);
    Status = NtfsKmtQueryInfo(Handle, Buffer,
                              FIELD_OFFSET(FILE_STREAM_INFORMATION, StreamName) + 7 * sizeof(WCHAR),
                              FileStreamInformation, NULL);
    ok_eq_hex(Status, Count > 1 ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS);

    NtfsKmtFree(Buffer);
    ZwClose(Handle);
}

static
VOID
TestStreams(
    _In_ PCWSTR Dir)
{
    NTSTATUS Status;
    WCHAR Base[MAX_PATH];
    WCHAR Path[MAX_PATH];
    UCHAR Small[64];
    PUCHAR Big;
    ULONG_PTR Info;
    ULONG_PTR Read;
    HANDLE Handle;
    FILE_STANDARD_INFORMATION Standard;
    STREAM_EXPECT Expect[3];
    static const CHAR MainData[] = "MAIN";
    static const CHAR S1Data[] = "stream one data";

    NtfsKmtJoin(Base, RTL_NUMBER_OF(Base), Dir, L"ads.txt");

    /* Base file with 4 bytes in the unnamed stream. */
    Status = WriteNew(Base, MainData, 4, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, FILE_CREATED);
    if (!NT_SUCCESS(Status))
        return;

    /* A brand-new named stream on an existing file reports FILE_CREATED. */
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls:s1", Base);
    Status = WriteNew(Path, S1Data, 15, &Info);
    if (skip(!NtfsKmtIsUnimplemented(Status) && Status != STATUS_OBJECT_NAME_INVALID,
             "Named streams not supported (0x%lx)\n", Status))
    {
        NtfsKmtDelete(Base);
        return;
    }
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, FILE_CREATED);

    /* FILE_CREATE of an existing stream collides. */
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE,
                           FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_COLLISION);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);

    /* A non-resident named stream, named with the explicit ":$DATA" type. */
    Big = NtfsKmtAlloc(BIG_STREAM_SIZE);
    if (Big == NULL)
        return;
    NtfsKmtFillPattern(Big, BIG_STREAM_SIZE, 0x5352, 0);
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls:S2:$DATA", Base);
    Status = WriteNew(Path, Big, BIG_STREAM_SIZE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, FILE_CREATED);

    /* Stream names are case-insensitive, and the base name too. */
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls\\ADS.TXT:S1", Dir);
    RtlZeroMemory(Small, sizeof(Small));
    Status = ReadAll(Path, Small, sizeof(Small), &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Read, 15);
    ok(RtlCompareMemory(Small, S1Data, 15) == 15, "stream s1 content mismatch\n");

    /* "file:s1:$DATA" names the same stream as "file:s1". */
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls:s1:$DATA", Base);
    RtlZeroMemory(Small, sizeof(Small));
    Status = ReadAll(Path, Small, sizeof(Small), &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Read, 15);

    /* Non-resident stream reads back intact. */
    RtlZeroMemory(Big, BIG_STREAM_SIZE);
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls:s2", Base);
    Status = ReadAll(Path, Big, BIG_STREAM_SIZE, &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Read, BIG_STREAM_SIZE);
    ok(NtfsKmtCheckPattern(Big, BIG_STREAM_SIZE, 0x5352, 0) == (SIZE_T)-1,
       "stream S2 content mismatch at %Iu\n",
       NtfsKmtCheckPattern(Big, BIG_STREAM_SIZE, 0x5352, 0));

    /* The unnamed stream is untouched by named-stream writes. */
    RtlZeroMemory(Small, sizeof(Small));
    Status = ReadAll(Base, Small, sizeof(Small), &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Read, 4);
    ok(RtlCompareMemory(Small, MainData, 4) == 4, "unnamed stream content mismatch\n");

    /* FileStandardInformation is per stream. */
    Status = NtfsKmtOpen(&Handle, Base, FILE_READ_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtQueryStandard(Handle, &Standard);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_longlong(Standard.EndOfFile.QuadPart, 4LL);
        ZwClose(Handle);
    }
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls:S2", Base);
    Status = NtfsKmtOpen(&Handle, Path, FILE_READ_ATTRIBUTES, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtQueryStandard(Handle, &Standard);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_longlong(Standard.EndOfFile.QuadPart, (LONGLONG)BIG_STREAM_SIZE);
        ok(!Standard.Directory, "stream reported as a directory\n");
        ZwClose(Handle);
    }

    /* Enumeration: ::$DATA, :s1:$DATA, :S2:$DATA. */
    Expect[0].Name = L"::$DATA";   Expect[0].Size = 4;
    Expect[1].Name = L":s1:$DATA"; Expect[1].Size = 15;
    Expect[2].Name = L":S2:$DATA"; Expect[2].Size = BIG_STREAM_SIZE;
    CheckStreamList(Base, Expect, 3);

    /* Missing stream: OBJECT_NAME_NOT_FOUND, not a create. */
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls:missing", Base);
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);

    /* Stream names cannot contain a further ':' segment that is not a type,
     * and an unknown attribute type is rejected (MS-FSA 2.1.5.1). */
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls:s1:$BOGUS", Base);
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_INVALID);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);

    /* Deleting a named stream removes only that stream. */
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls:s1", Base);
    Status = NtfsKmtDelete(Path);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);
    Expect[1].Name = L":S2:$DATA"; Expect[1].Size = BIG_STREAM_SIZE;
    CheckStreamList(Base, Expect, 2);

    /* The base file and the remaining stream survive the stream delete. */
    RtlZeroMemory(Small, sizeof(Small));
    Status = ReadAll(Base, Small, sizeof(Small), &Read);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Read, 4);

    /* Opening a stream with FILE_DELETE_ON_CLOSE deletes only the stream. */
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls:tmp", Base);
    Status = NtfsKmtCreate(&Handle, NULL, Path, DELETE | GENERIC_WRITE, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_CREATE, FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE,
                           NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);
    CheckStreamList(Base, Expect, 2);

    /* Deleting the file deletes all of its streams. */
    Status = NtfsKmtDelete(Base);
    ok_eq_hex(Status, STATUS_SUCCESS);
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls:S2", Base);
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);

    /* Re-creating the same name starts with a single unnamed stream. */
    Status = WriteNew(Base, MainData, 4, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    CheckStreamList(Base, Expect, 1);
    NtfsKmtDelete(Base);

    NtfsKmtFree(Big);
}

/* Named data streams on a directory are legal NTFS (MS-FSA: a stream open of
 * a directory with a non-empty stream name opens a data stream). */
static
VOID
TestDirectoryStreams(
    _In_ PCWSTR Dir)
{
    NTSTATUS Status;
    WCHAR SubDir[MAX_PATH];
    WCHAR Path[MAX_PATH];
    UCHAR Small[16];
    ULONG_PTR Info;
    ULONG_PTR Read;
    HANDLE Handle;

    NtfsKmtJoin(SubDir, RTL_NUMBER_OF(SubDir), Dir, L"adsdir");
    Status = NtfsKmtCreateDirectory(SubDir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls:dirstream", SubDir);
    Status = WriteNew(Path, "dir!", 4, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_ulongptr(Info, FILE_CREATED);
        Status = ReadAll(Path, Small, sizeof(Small), &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Read, 4);
    }

    /* "dir::$DATA" names a data stream the directory does not have. */
    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"%ls::$DATA", SubDir);
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, 0);
    ok_eq_hex(Status, STATUS_FILE_IS_A_DIRECTORY);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);

    NtfsKmtDeleteTree(SubDir);
}

START_TEST(NtfsStreams)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_streams", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    TestStreams(Dir);
    TestDirectoryStreams(Dir);

    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
