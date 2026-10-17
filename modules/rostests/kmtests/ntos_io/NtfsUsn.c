/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS USN change journal
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box $UsnJrnl coverage: FSCTL_CREATE/QUERY/READ/DELETE_USN_JOURNAL,
 * FSCTL_READ_FILE_USN_DATA and FSCTL_ENUM_USN_DATA, plus the records a
 * create / write / rename / delete sequence must produce.
 *
 * Replaces the host tests test_fsctl_usn, test_fsctl_usn_advanced and
 * test_usn_workers, which drove the usn.c workers on a synthetic journal.
 *
 * Windows reference: the FSCTL_*_USN_* documentation and MS-FSCC 2.3
 * (USN_RECORD_V2, USN_JOURNAL_DATA_V0, MFT_ENUM_DATA_V0).
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

#define READ_BUFFER_SIZE    (64 * 1024)

typedef struct _USN_SEEN
{
    ULONG ReasonsOld;           /* reasons on records carrying the old name */
    ULONG ReasonsNew;           /* reasons on records carrying the new name */
    ULONG Records;
    ULONGLONG FileRef;
    BOOLEAN FileRefMismatch;
    BOOLEAN ParentMismatch;
    BOOLEAN UsnNotIncreasing;
} USN_SEEN, *PUSN_SEEN;

static
BOOLEAN
NameIs(
    _In_ PUSN_RECORD Record,
    _In_ PCWSTR Name)
{
    SIZE_T Length = wcslen(Name) * sizeof(WCHAR);

    return Record->FileNameLength == Length &&
           RtlCompareMemory((PUCHAR)Record + Record->FileNameOffset, Name, Length) == Length;
}

/* Find the record of Name in an FSCTL_ENUM_USN_DATA walk of the whole MFT. */
static
VOID
TestEnumUsnData(
    _In_ HANDLE Volume,
    _In_ PCWSTR Name,
    _In_ ULONGLONG FileRef,
    _In_ ULONGLONG ParentRef)
{
    MFT_ENUM_DATA Enum;
    PUCHAR Buffer;
    NTSTATUS Status;
    ULONG_PTR Info;
    ULONG Calls = 0, Found = 0, Records = 0;
    BOOLEAN BadRecord = FALSE;

    Buffer = NtfsKmtAlloc(READ_BUFFER_SIZE);
    if (Buffer == NULL)
        return;

    Enum.StartFileReferenceNumber = 0;
    Enum.LowUsn = 0;
    Enum.HighUsn = MAXLONGLONG;
    for (;;)
    {
        PUCHAR Cursor;

        Status = NtfsKmtFsctl(Volume, FSCTL_ENUM_USN_DATA, &Enum, sizeof(Enum),
                              Buffer, READ_BUFFER_SIZE, &Info);
        if (Calls == 0 &&
            skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_ENUM_USN_DATA not supported (0x%lx)\n", Status))
        {
            NtfsKmtFree(Buffer);
            return;
        }
        Calls++;
        /* ERROR_HANDLE_EOF ends the walk. */
        if (Status == STATUS_END_OF_FILE)
            break;
        if (!NT_SUCCESS(Status))
        {
            ok(FALSE, "FSCTL_ENUM_USN_DATA failed 0x%lx after %lu calls\n", Status, Calls);
            break;
        }
        ok(Info >= sizeof(USN), "ENUM_USN_DATA returned %Iu bytes\n", Info);
        if (Info < sizeof(USN))
            break;

        for (Cursor = Buffer + sizeof(USN); Cursor < Buffer + Info;)
        {
            PUSN_RECORD Record = (PUSN_RECORD)Cursor;

            if (Record->RecordLength < FIELD_OFFSET(USN_RECORD, FileName) ||
                Cursor + Record->RecordLength > Buffer + Info ||
                Record->MajorVersion != 2)
            {
                BadRecord = TRUE;
                break;
            }
            Records++;
            if (NameIs(Record, Name))
            {
                Found++;
                ok(Record->FileReferenceNumber == FileRef,
                   "ENUM record FRN %I64x, expected %I64x\n", Record->FileReferenceNumber, FileRef);
                ok(Record->ParentFileReferenceNumber == ParentRef,
                   "ENUM record parent %I64x, expected %I64x\n",
                   Record->ParentFileReferenceNumber, ParentRef);
            }
            Cursor += Record->RecordLength;
        }
        if (BadRecord)
            break;

        /* The next call continues from the returned file reference. */
        if (*(PULONGLONG)Buffer <= Enum.StartFileReferenceNumber && Info == sizeof(USN))
            break;
        Enum.StartFileReferenceNumber = *(PULONGLONG)Buffer;
        if (Calls > 10000)
            break;
    }
    ok(!BadRecord, "malformed USN_RECORD in FSCTL_ENUM_USN_DATA output\n");
    ok_eq_hex(Status, STATUS_END_OF_FILE);
    ok(Records >= 3, "only %lu records enumerated\n", Records);
    ok_eq_ulong(Found, 1UL);
    NtfsKmtFree(Buffer);
}

static
VOID
CollectRecords(
    _In_ HANDLE Volume,
    _In_ ULONGLONG JournalId,
    _In_ USN StartUsn,
    _In_ PCWSTR OldName,
    _In_ PCWSTR NewName,
    _Inout_ PUSN_SEEN Seen)
{
    READ_USN_JOURNAL_DATA Read;
    PUCHAR Buffer;
    NTSTATUS Status;
    ULONG_PTR Info;
    ULONG Calls = 0;
    USN LastUsn = 0;

    Buffer = NtfsKmtAlloc(READ_BUFFER_SIZE);
    if (Buffer == NULL)
        return;

    RtlZeroMemory(&Read, sizeof(Read));
    Read.StartUsn = StartUsn;
    Read.ReasonMask = 0xFFFFFFFF;
    Read.ReturnOnlyOnClose = FALSE;
    Read.Timeout = 0;
    Read.BytesToWaitFor = 0;
    Read.UsnJournalID = JournalId;
    for (;;)
    {
        PUCHAR Cursor;

        Status = NtfsKmtFsctl(Volume, FSCTL_READ_USN_JOURNAL, &Read, sizeof(Read),
                              Buffer, READ_BUFFER_SIZE, &Info);
        ok(Status == STATUS_SUCCESS, "FSCTL_READ_USN_JOURNAL 0x%lx\n", Status);
        if (!NT_SUCCESS(Status) || Info <= sizeof(USN))
            break;

        for (Cursor = Buffer + sizeof(USN); Cursor < Buffer + Info;)
        {
            PUSN_RECORD Record = (PUSN_RECORD)Cursor;

            if (Record->RecordLength < FIELD_OFFSET(USN_RECORD, FileName) ||
                Cursor + Record->RecordLength > Buffer + Info)
            {
                ok(FALSE, "malformed USN_RECORD in FSCTL_READ_USN_JOURNAL output\n");
                break;
            }
            ok_eq_uint(Record->MajorVersion, 2);
            if (Record->Usn < LastUsn)
                Seen->UsnNotIncreasing = TRUE;
            LastUsn = Record->Usn;

            if (NameIs(Record, OldName) || NameIs(Record, NewName))
            {
                if (Seen->Records == 0)
                    Seen->FileRef = Record->FileReferenceNumber;
                else if (Record->FileReferenceNumber != Seen->FileRef)
                    Seen->FileRefMismatch = TRUE;
                Seen->Records++;
                if (NameIs(Record, OldName))
                    Seen->ReasonsOld |= Record->Reason;
                else
                    Seen->ReasonsNew |= Record->Reason;
            }
            Cursor += Record->RecordLength;
        }
        Read.StartUsn = *(USN *)Buffer;
        if (++Calls > 1000)
            break;
    }
    NtfsKmtFree(Buffer);
}

/* Locate the journal through the documented MFT enumeration, then inspect
 * its data attribute through GET_NTFS_FILE_RECORD. No pathname access to
 * hidden NTFS metadata or estimate of unrelated allocations is needed. */
static VOID
CheckJournalAllocation(HANDLE Volume, PUSN_JOURNAL_DATA Journal, ULONG RecordBytes, ULONGLONG Unit)
{
    MFT_ENUM_DATA Enum;
    PNTFS_FILE_RECORD_OUTPUT_BUFFER Output = NULL;
    NTFS_FILE_RECORD_INPUT_BUFFER Input;
    PUCHAR Buffer = NULL, Cursor, Attribute;
    ULONG_PTR Returned;
    ULONG Calls = 0, Offset, Type, Length;
    USHORT AttributeOffset, NameOffset, Flags;
    ULONGLONG Reference = 0, Size, Allocated;
    BOOLEAN Found = FALSE;
    NTSTATUS Status;

    Buffer = NtfsKmtAlloc(READ_BUFFER_SIZE);
    if (Buffer == NULL) return;
    RtlZeroMemory(&Enum, sizeof(Enum));
    Enum.HighUsn = MAXLONGLONG;
    while (Calls++ < 1000 && Reference == 0)
    {
        Status = NtfsKmtFsctl(Volume, FSCTL_ENUM_USN_DATA, &Enum, sizeof(Enum), Buffer, READ_BUFFER_SIZE, &Returned);
        if (Status == STATUS_END_OF_FILE || Status == STATUS_NO_MORE_FILES) break;
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status) || Returned <= sizeof(ULONGLONG)) break;
        for (Cursor = Buffer + sizeof(ULONGLONG); Cursor < Buffer + Returned;)
        {
            PUSN_RECORD Record = (PVOID)Cursor;
            if (Record->RecordLength < FIELD_OFFSET(USN_RECORD, FileName) ||
                Record->RecordLength > (ULONG_PTR)(Buffer + Returned - Cursor)) break;
            if (NameIs(Record, L"$UsnJrnl"))
            {
                Reference = Record->FileReferenceNumber & 0x0000ffffffffffffULL;
                break;
            }
            Cursor += Record->RecordLength;
        }
        Enum.StartFileReferenceNumber = *(PULONGLONG)Buffer;
    }
    ok(Reference != 0, "journal file missing from MFT enumeration\n");
    if (Reference == 0) goto CleanupAllocation;
    Length = FIELD_OFFSET(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + RecordBytes;
    Output = NtfsKmtAlloc(Length);
    if (Output == NULL) goto CleanupAllocation;
    Input.FileReferenceNumber.QuadPart = Reference;
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_FILE_RECORD, &Input, sizeof(Input), Output, Length, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto CleanupAllocation;
    ok(Output->FileRecordLength >= 24 && Output->FileRecordLength <= RecordBytes, "invalid journal record length\n");
    if (Output->FileRecordLength < 24 || Output->FileRecordLength > RecordBytes) goto CleanupAllocation;
    RtlCopyMemory(&AttributeOffset, Output->FileRecordBuffer + 20, sizeof(AttributeOffset));
    for (Offset = AttributeOffset; Offset + 16 <= Output->FileRecordLength; Offset += Length)
    {
        Attribute = Output->FileRecordBuffer + Offset;
        RtlCopyMemory(&Type, Attribute, sizeof(Type));
        if (Type == MAXULONG) break;
        RtlCopyMemory(&Length, Attribute + 4, sizeof(Length));
        ok(Length >= 16 && Length <= Output->FileRecordLength - Offset, "invalid journal attribute\n");
        if (Length < 16 || Length > Output->FileRecordLength - Offset) break;
        RtlCopyMemory(&NameOffset, Attribute + 10, sizeof(NameOffset));
        if (Type != 0x80 || Attribute[9] != 2 || NameOffset > Length ||
            Length - NameOffset < 2 * sizeof(WCHAR) || !RtlEqualMemory(Attribute + NameOffset, L"$J", 2 * sizeof(WCHAR)))
            continue;
        Found = TRUE;
        RtlCopyMemory(&Flags, Attribute + 12, sizeof(Flags));
        ok(Attribute[8] != 0 && Length >= 72 && (Flags & 0x8000), "$J is not nonresident sparse data\n");
        if (Attribute[8] != 0 && Length >= 72 && (Flags & 0x8000))
        {
            RtlCopyMemory(&Size, Attribute + 48, sizeof(Size));
            RtlCopyMemory(&Allocated, Attribute + 64, sizeof(Allocated));
            ok_eq_ulonglong(Size, Journal->NextUsn);
            ok(Allocated <= max(Journal->MaximumSize, Unit) + Journal->AllocationDelta + Unit,
               "journal backing allocation is unbounded: %I64u bytes\n", Allocated);
        }
        break;
    }
    ok(Found, "journal record has no $J stream\n");
CleanupAllocation:
    if (Output != NULL) NtfsKmtFree(Output);
    NtfsKmtFree(Buffer);
}

static VOID
TestJournalTrimming(PCWSTR Root, WCHAR Letter)
{
    WCHAR Dir[MAX_PATH], Path[MAX_PATH], Name[181];
    HANDLE Volume = NULL, File = NULL;
    CREATE_USN_JOURNAL_DATA Create;
    DELETE_USN_JOURNAL_DATA Delete;
    USN_JOURNAL_DATA Before = {0}, After, Reloaded;
    NTFS_VOLUME_DATA_BUFFER OldData;
    READ_USN_JOURNAL_DATA Read;
    USN_SEEN Seen;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    UCHAR Out[64];
    ULONGLONG Unit;
    ULONG i, Iterations, OldDirty, NewDirty;
    CHAR Byte;
    BOOLEAN Created = FALSE;
    NTSTATUS Status;

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_usn_trim", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtFsctl(Volume, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &OldDirty, sizeof(OldDirty), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtFsctl(Volume, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &OldData, sizeof(OldData), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Create.MaximumSize = 64 * 1024;
    Create.AllocationDelta = 64 * 1024;
    Status = NtfsKmtFsctl(Volume, FSCTL_CREATE_USN_JOURNAL, &Create, sizeof(Create), NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Created = TRUE;
    Status = NtfsKmtFsctl(Volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &Before, sizeof(Before), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    for (i = 0; i < RTL_NUMBER_OF(Name) - 1; i++) Name[i] = L'T';
    Name[RTL_NUMBER_OF(Name) - 1] = UNICODE_NULL;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, Name);
    Status = NtfsKmtCreateFileWithData(Path, "x", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Offset.QuadPart = 0;
    Unit = max((ULONGLONG)OldData.BytesPerCluster * 16, 4096);
    Iterations = (ULONG)((Before.MaximumSize + Before.AllocationDelta + Unit) /
                         ALIGN_UP_BY(FIELD_OFFSET(USN_RECORD, FileName) + (RTL_NUMBER_OF(Name) - 1) * sizeof(WCHAR), 8)) + 32;
    for (i = 0; i < Iterations; i++)
    {
        Status = NtfsKmtOpen(&File, Path, FILE_WRITE_DATA, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) goto Cleanup;
        Byte = 'a' + i % 26;
        Status = ZwWriteFile(File, NULL, NULL, NULL, &IoStatus, &Byte, 1, &Offset, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(File); File = NULL;
        if (!NT_SUCCESS(Status)) goto Cleanup;
    }
    Status = NtfsKmtFlush(Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtFsctl(Volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &After, sizeof(After), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Unit = max((ULONGLONG)OldData.BytesPerCluster * 16, 4096);
    ok(After.FirstUsn > Before.FirstUsn, "journal never trimmed: first %I64d next %I64d\n", After.FirstUsn, After.NextUsn);
    ok(After.NextUsn > Before.NextUsn, "journal USNs did not advance\n");
    ok_eq_longlong(After.UsnJournalID, Before.UsnJournalID);
    ok_eq_longlong(After.LowestValidUsn, Before.LowestValidUsn);
    ok(After.NextUsn - After.FirstUsn <= max(After.MaximumSize, Unit),
       "retained journal exceeds checkpoint target: %I64d bytes\n", After.NextUsn - After.FirstUsn);
    ok(After.FirstUsn % Unit == 0, "trim split an allocation unit\n");
    RtlZeroMemory(&Read, sizeof(Read));
    Read.StartUsn = Before.FirstUsn;
    Read.ReasonMask = MAXULONG;
    Read.UsnJournalID = After.UsnJournalID;
    Status = NtfsKmtFsctl(Volume, FSCTL_READ_USN_JOURNAL, &Read, sizeof(Read), Out, sizeof(Out), NULL);
    ok_eq_hex(Status, STATUS_JOURNAL_ENTRY_DELETED);
    RtlZeroMemory(&Seen, sizeof(Seen));
    CollectRecords(Volume, After.UsnJournalID, After.FirstUsn, Name, Name, &Seen);
    ok(Seen.Records != 0, "no retained records for the active file\n");
    ok(Seen.ReasonsOld & USN_REASON_DATA_OVERWRITE, "lost retained overwrite records\n");
    ok(Seen.ReasonsOld & USN_REASON_CLOSE, "lost retained close records\n");
    ok(!Seen.UsnNotIncreasing && !Seen.FileRefMismatch, "invalid retained journal sequence\n");

    CheckJournalAllocation(Volume, &After, OldData.BytesPerFileRecordSegment, Unit);
    Status = NtfsKmtFsctl(Volume, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &NewDirty, sizeof(NewDirty), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(NewDirty, OldDirty);

    Status = NtfsKmtFsctl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtFsctl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    ZwClose(Volume); Volume = NULL;
    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtFsctl(Volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &Reloaded, sizeof(Reloaded), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    ok_eq_longlong(Reloaded.UsnJournalID, After.UsnJournalID);
    ok_eq_longlong(Reloaded.LowestValidUsn, After.LowestValidUsn);
    ok_eq_longlong(Reloaded.FirstUsn, After.FirstUsn);
    ok_eq_longlong(Reloaded.NextUsn, After.NextUsn);
    Status = NtfsKmtFsctl(Volume, FSCTL_READ_USN_JOURNAL, &Read, sizeof(Read), Out, sizeof(Out), NULL);
    ok_eq_hex(Status, STATUS_JOURNAL_ENTRY_DELETED);
    RtlZeroMemory(&Seen, sizeof(Seen));
    CollectRecords(Volume, Reloaded.UsnJournalID, Reloaded.FirstUsn, Name, Name, &Seen);
    ok(Seen.Records != 0 && !Seen.UsnNotIncreasing, "lost retained journal across remount\n");
Cleanup:
    if (File != NULL) ZwClose(File);
    if (Created && Volume != NULL)
    {
        Delete.UsnJournalID = Before.UsnJournalID;
        Delete.DeleteFlags = USN_DELETE_FLAG_DELETE | USN_DELETE_FLAG_NOTIFY;
        Status = NtfsKmtFsctl(Volume, FSCTL_DELETE_USN_JOURNAL, &Delete, sizeof(Delete), NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    if (Volume != NULL) ZwClose(Volume);
    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

START_TEST(NtfsUsn)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    WCHAR OldPath[MAX_PATH];
    WCHAR NewPath[MAX_PATH];
    WCHAR Letter;
    NTSTATUS Status;
    HANDLE Volume;
    HANDLE Handle;
    HANDLE DirHandle;
    CREATE_USN_JOURNAL_DATA Create;
    USN_JOURNAL_DATA Journal, Journal2;
    BOOLEAN JournalWasActive = FALSE;
    BOOLEAN HaveJournal = FALSE;
    ULONG_PTR Info;
    ULONGLONG FileRef = 0, DirRef = 0;
    USN StartUsn = 0;

    if (!NtfsKmtFindVolume(Root, &Letter))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_usn", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* A fresh volume has no journal: QUERY reports STATUS_JOURNAL_NOT_ACTIVE. */
    Status = NtfsKmtFsctl(Volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &Journal, sizeof(Journal), &Info);
    if (!skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_QUERY_USN_JOURNAL not supported (0x%lx)\n", Status))
    {
        ok(Status == STATUS_JOURNAL_NOT_ACTIVE || Status == STATUS_SUCCESS,
           "QUERY_USN_JOURNAL before create: 0x%lx\n", Status);
        JournalWasActive = NT_SUCCESS(Status);
        if (JournalWasActive)
            trace("journal already active (left by a previous run?)\n");
    }

    /* Create (or re-size) the journal. */
    Create.MaximumSize = 1024 * 1024;
    Create.AllocationDelta = 256 * 1024;
    Status = NtfsKmtFsctl(Volume, FSCTL_CREATE_USN_JOURNAL, &Create, sizeof(Create), NULL, 0, NULL);
    if (!skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_CREATE_USN_JOURNAL not supported (0x%lx)\n", Status))
    {
        ok_eq_hex(Status, STATUS_SUCCESS);
        RtlZeroMemory(&Journal, sizeof(Journal));
        Status = NtfsKmtFsctl(Volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &Journal, sizeof(Journal), &Info);
        if (!skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_QUERY_USN_JOURNAL not supported (0x%lx)\n", Status))
        {
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok_eq_ulongptr(Info, sizeof(USN_JOURNAL_DATA));
            ok(Journal.UsnJournalID != 0, "UsnJournalID is 0\n");
            ok(Journal.FirstUsn <= Journal.NextUsn, "FirstUsn %I64d > NextUsn %I64d\n",
               Journal.FirstUsn, Journal.NextUsn);
            ok(Journal.LowestValidUsn <= Journal.FirstUsn, "LowestValidUsn %I64d > FirstUsn %I64d\n",
               Journal.LowestValidUsn, Journal.FirstUsn);
            ok(Journal.MaximumSize >= Create.MaximumSize, "MaximumSize %I64u\n", Journal.MaximumSize);
            ok(Journal.MaxUsn > Journal.NextUsn, "MaxUsn %I64d\n", Journal.MaxUsn);
            HaveJournal = NT_SUCCESS(Status);
            StartUsn = Journal.NextUsn;

            /* Creating it again keeps the same journal (same ID). */
            Status = NtfsKmtFsctl(Volume, FSCTL_CREATE_USN_JOURNAL, &Create, sizeof(Create), NULL, 0, NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            Status = NtfsKmtFsctl(Volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &Journal2, sizeof(Journal2), NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok(Journal2.UsnJournalID == Journal.UsnJournalID, "journal ID changed on re-create\n");
        }
    }

    /* create + write + close, rename, delete. */
    NtfsKmtJoin(OldPath, RTL_NUMBER_OF(OldPath), Dir, L"usn_old.txt");
    NtfsKmtJoin(NewPath, RTL_NUMBER_OF(NewPath), Dir, L"usn_new.txt");
    Status = NtfsKmtOpen(&DirHandle, Dir, FILE_READ_ATTRIBUTES, FILE_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        DirRef = NtfsKmtQueryFileId(DirHandle);
        ZwClose(DirHandle);
    }
    Status = NtfsKmtCreate(&Handle, NULL, OldPath, GENERIC_READ | GENERIC_WRITE | DELETE, 0,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_CREATE,
                           FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        FileRef = NtfsKmtQueryFileId(Handle);
        Status = NtfsKmtWrite(Handle, 0, "usn data", 8, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);

        /* FSCTL_READ_FILE_USN_DATA describes this file. */
        if (HaveJournal)
        {
            PUSN_RECORD Record = NtfsKmtAlloc(1024);
            if (Record != NULL)
            {
                Status = NtfsKmtFsctl(Handle, FSCTL_READ_FILE_USN_DATA, NULL, 0, Record, 1024, &Info);
                if (!skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_READ_FILE_USN_DATA not supported (0x%lx)\n", Status))
                {
                    ok_eq_hex(Status, STATUS_SUCCESS);
                    ok_eq_uint(Record->MajorVersion, 2);
                    ok(Record->FileReferenceNumber == FileRef, "READ_FILE_USN_DATA FRN %I64x, expected %I64x\n",
                       Record->FileReferenceNumber, FileRef);
                    ok(Record->ParentFileReferenceNumber == DirRef, "READ_FILE_USN_DATA parent %I64x, expected %I64x\n",
                       Record->ParentFileReferenceNumber, DirRef);
                    ok(NameIs(Record, L"usn_old.txt"), "READ_FILE_USN_DATA name mismatch\n");
                    ok(Record->Usn >= StartUsn, "file USN %I64d below journal start %I64d\n", Record->Usn, StartUsn);
                }
                NtfsKmtFree(Record);
            }
        }
        ZwClose(Handle);
    }

    /* MFT enumeration finds the new file by name, FRN and parent. */
    TestEnumUsnData(Volume, L"usn_old.txt", FileRef, DirRef);

    Status = NtfsKmtOpen(&Handle, OldPath, DELETE, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtRename(Handle, NewPath, FALSE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Handle);
    }
    Status = NtfsKmtDelete(NewPath);
    ok_eq_hex(Status, STATUS_SUCCESS);

    if (HaveJournal)
    {
        USN_SEEN Seen;

        RtlZeroMemory(&Seen, sizeof(Seen));
        CollectRecords(Volume, Journal.UsnJournalID, StartUsn, L"usn_old.txt", L"usn_new.txt", &Seen);
        ok(Seen.Records >= 4, "only %lu journal records for the test file\n", Seen.Records);
        ok(!Seen.FileRefMismatch, "journal records disagree on the file reference\n");
        ok(Seen.FileRef == FileRef, "journal FRN %I64x, expected %I64x\n", Seen.FileRef, FileRef);
        ok(!Seen.UsnNotIncreasing, "USNs not increasing\n");
        ok(Seen.ReasonsOld & USN_REASON_FILE_CREATE, "no FILE_CREATE record (old 0x%lx)\n", Seen.ReasonsOld);
        ok(Seen.ReasonsOld & USN_REASON_DATA_EXTEND, "no DATA_EXTEND record (old 0x%lx)\n", Seen.ReasonsOld);
        ok(Seen.ReasonsOld & USN_REASON_CLOSE, "no CLOSE record (old 0x%lx)\n", Seen.ReasonsOld);
        ok(Seen.ReasonsOld & USN_REASON_RENAME_OLD_NAME, "no RENAME_OLD_NAME record (old 0x%lx)\n", Seen.ReasonsOld);
        ok(Seen.ReasonsNew & USN_REASON_RENAME_NEW_NAME, "no RENAME_NEW_NAME record (new 0x%lx)\n", Seen.ReasonsNew);
        ok(Seen.ReasonsNew & USN_REASON_FILE_DELETE, "no FILE_DELETE record (new 0x%lx)\n", Seen.ReasonsNew);

        /* A journal ID that does not match the active journal is rejected
         * (the exact status is not documented, only that the call fails). */
        {
            READ_USN_JOURNAL_DATA Read;
            UCHAR Out[64];

            RtlZeroMemory(&Read, sizeof(Read));
            Read.StartUsn = StartUsn;
            Read.ReasonMask = 0xFFFFFFFF;
            Read.UsnJournalID = Journal.UsnJournalID + 1;
            Status = NtfsKmtFsctl(Volume, FSCTL_READ_USN_JOURNAL, &Read, sizeof(Read), Out, sizeof(Out), NULL);
            ok(!NT_SUCCESS(Status), "READ_USN_JOURNAL with a stale journal ID: 0x%lx\n", Status);
        }

        /* Delete the journal again unless it predates this test. */
        if (!JournalWasActive)
        {
            DELETE_USN_JOURNAL_DATA Delete;

            Delete.UsnJournalID = Journal.UsnJournalID;
            Delete.DeleteFlags = USN_DELETE_FLAG_DELETE | USN_DELETE_FLAG_NOTIFY;
            Status = NtfsKmtFsctl(Volume, FSCTL_DELETE_USN_JOURNAL, &Delete, sizeof(Delete), NULL, 0, NULL);
            ok_eq_hex(Status, STATUS_SUCCESS);
            Status = NtfsKmtFsctl(Volume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &Journal2, sizeof(Journal2), NULL);
            ok_eq_hex(Status, STATUS_JOURNAL_NOT_ACTIVE);
        }
    }

    ZwClose(Volume);
    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (HaveJournal && !JournalWasActive)
        TestJournalTrimming(Root, Letter);
}
