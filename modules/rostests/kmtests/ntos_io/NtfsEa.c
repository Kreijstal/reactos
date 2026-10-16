/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS extended attributes
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box EA coverage through ZwSetEaFile / ZwQueryEaFile, the EaBuffer of
 * ZwCreateFile and FileEaInformation: first EA on a file without any, add,
 * replace, delete by zero-length value, full and by-name queries, the
 * single-entry cursor, upper-cased names, an inconsistent list, persistence
 * across reopen.
 *
 * Replaces the EA half of the host test test_ntfs_metadata_features.
 *
 * Windows reference: Wine ntdll/file.c test_query_ea (STATUS_NO_EAS_ON_FILE),
 * MS-FSCC 2.4.15 FILE_FULL_EA_INFORMATION, IoCheckEaBufferValidity.
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

#define EA_BUFFER_SIZE  4096

/* Append one FILE_FULL_EA_INFORMATION; returns the new total length. */
static
ULONG
AddEa(
    _Inout_updates_bytes_(EA_BUFFER_SIZE) PUCHAR Buffer,
    _In_ ULONG Used,
    _Inout_ PULONG LastOffset,
    _In_ PCSTR Name,
    _In_reads_bytes_(ValueLength) const VOID *Value,
    _In_ USHORT ValueLength)
{
    PFILE_FULL_EA_INFORMATION Ea;
    ULONG NameLength = (ULONG)strlen(Name);
    ULONG Size = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + NameLength + 1 + ValueLength;
    ULONG Start = ALIGN_UP_BY(Used, sizeof(ULONG));

    if (Used != 0)
        ((PFILE_FULL_EA_INFORMATION)(Buffer + *LastOffset))->NextEntryOffset = Start - *LastOffset;
    Ea = (PFILE_FULL_EA_INFORMATION)(Buffer + Start);
    Ea->NextEntryOffset = 0;
    Ea->Flags = 0;
    Ea->EaNameLength = (UCHAR)NameLength;
    Ea->EaValueLength = ValueLength;
    RtlCopyMemory(Ea->EaName, Name, NameLength + 1);
    if (ValueLength != 0)
        RtlCopyMemory(Ea->EaName + NameLength + 1, Value, ValueLength);
    *LastOffset = Start;
    return Start + Size;
}

static
NTSTATUS
SetEas(
    _In_ HANDLE Handle,
    _In_ PVOID Buffer,
    _In_ ULONG Length)
{
    IO_STATUS_BLOCK IoStatus;

    return ZwSetEaFile(Handle, &IoStatus, Buffer, Length);
}

static
NTSTATUS
QueryEas(
    _In_ HANDLE Handle,
    _Out_writes_bytes_(EA_BUFFER_SIZE) PVOID Buffer,
    _In_ BOOLEAN Single,
    _In_opt_ PVOID EaList,
    _In_ ULONG EaListLength,
    _In_ BOOLEAN Restart,
    _Out_opt_ PULONG_PTR Info)
{
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    RtlZeroMemory(Buffer, EA_BUFFER_SIZE);
    IoStatus.Information = 0;
    Status = ZwQueryEaFile(Handle, &IoStatus, Buffer, EA_BUFFER_SIZE, Single,
                           EaList, EaListLength, NULL, Restart);
    if (Info != NULL)
        *Info = IoStatus.Information;
    return Status;
}

/* Find EA Name in a returned list; returns it or NULL. */
static
PFILE_FULL_EA_INFORMATION
FindEa(
    _In_ PUCHAR Buffer,
    _In_ PCSTR Name,
    _Out_opt_ PULONG Count)
{
    PFILE_FULL_EA_INFORMATION Ea = (PFILE_FULL_EA_INFORMATION)Buffer;
    PFILE_FULL_EA_INFORMATION Found = NULL;
    ULONG n = 0;

    for (;;)
    {
        n++;
        if (Ea->EaNameLength == strlen(Name) &&
            RtlCompareMemory(Ea->EaName, Name, Ea->EaNameLength) == Ea->EaNameLength)
            Found = Ea;
        if (Ea->NextEntryOffset == 0 || n > 64)
            break;
        Ea = (PFILE_FULL_EA_INFORMATION)((PUCHAR)Ea + Ea->NextEntryOffset);
    }
    if (Count != NULL)
        *Count = n;
    return Found;
}

static
ULONG
QueryEaSize(
    _In_ HANDLE Handle)
{
    FILE_EA_INFORMATION Info;

    Info.EaSize = 0xFFFFFFFF;
    ok_eq_hex(NtfsKmtQueryInfo(Handle, &Info, sizeof(Info), FileEaInformation, NULL), STATUS_SUCCESS);
    return Info.EaSize;
}

static
VOID
TestEas(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    PUCHAR In, Out;
    ULONG Used, Last, Count;
    ULONG_PTR Info;
    PFILE_FULL_EA_INFORMATION Ea;
    UCHAR Big[300];

    In = NtfsKmtAlloc(EA_BUFFER_SIZE);
    Out = NtfsKmtAlloc(EA_BUFFER_SIZE);
    if (In == NULL || Out == NULL)
        goto Free;
    RtlFillMemory(Big, sizeof(Big), 0x5A);

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"ea.txt");
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE | FILE_READ_EA | FILE_WRITE_EA,
                           0, FILE_SHARE_READ, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Free;

    /* No EAs yet (Wine test_query_ea). */
    Status = QueryEas(Handle, Out, FALSE, NULL, 0, TRUE, NULL);
    ok_eq_hex(Status, STATUS_NO_EAS_ON_FILE);
    ok_eq_ulong(QueryEaSize(Handle), 0UL);

    /* First EAs on a file that has none. */
    Last = 0;
    Used = AddEa(In, 0, &Last, "kmtEa1", "value1", 6);
    Used = AddEa(In, Used, &Last, "Second", Big, sizeof(Big));
    Status = SetEas(Handle, In, Used);
    if (skip(!NtfsKmtIsUnimplemented(Status), "Setting EAs not supported (0x%lx)\n", Status))
    {
        ZwClose(Handle);
        goto Free;
    }
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(QueryEaSize(Handle) != 0, "FileEaInformation.EaSize is 0 with EAs present\n");

    /* Full query: both, names upper-cased by the file system. */
    Status = QueryEas(Handle, Out, FALSE, NULL, 0, TRUE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Ea = FindEa(Out, "KMTEA1", &Count);
        ok_eq_ulong(Count, 2UL);
        ok(Ea != NULL, "KMTEA1 not returned (names must be upper case)\n");
        if (Ea != NULL)
        {
            ok_eq_uint(Ea->EaValueLength, 6);
            ok(RtlCompareMemory(Ea->EaName + Ea->EaNameLength + 1, "value1", 6) == 6, "KMTEA1 value differs\n");
        }
        Ea = FindEa(Out, "SECOND", NULL);
        ok(Ea != NULL, "SECOND not returned\n");
        if (Ea != NULL)
        {
            ok_eq_uint(Ea->EaValueLength, sizeof(Big));
            ok(RtlCompareMemory(Ea->EaName + Ea->EaNameLength + 1, Big, sizeof(Big)) == sizeof(Big),
               "SECOND value differs\n");
        }
    }

    /* Query by name (case-insensitive EaList). */
    {
        UCHAR List[32];
        PFILE_GET_EA_INFORMATION Get = (PFILE_GET_EA_INFORMATION)List;

        RtlZeroMemory(List, sizeof(List));
        Get->EaNameLength = 6;
        RtlCopyMemory(Get->EaName, "second", 7);
        Status = QueryEas(Handle, Out, FALSE, Get,
                          FIELD_OFFSET(FILE_GET_EA_INFORMATION, EaName) + 7, TRUE, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            Ea = FindEa(Out, "SECOND", &Count);
            ok_eq_ulong(Count, 1UL);
            ok(Ea != NULL && Ea->EaValueLength == sizeof(Big), "query by name failed\n");
        }
    }

    /* Single-entry cursor: first, second, then no more. */
    Status = QueryEas(Handle, Out, TRUE, NULL, 0, TRUE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(((PFILE_FULL_EA_INFORMATION)Out)->NextEntryOffset == 0, "single entry has a successor\n");
    Status = QueryEas(Handle, Out, TRUE, NULL, 0, FALSE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = QueryEas(Handle, Out, TRUE, NULL, 0, FALSE, NULL);
    ok_eq_hex(Status, STATUS_NO_MORE_EAS);

    /* Replace one value, add a third, delete the first (zero-length). */
    Last = 0;
    Used = AddEa(In, 0, &Last, "SECOND", "short", 5);
    Used = AddEa(In, Used, &Last, "third", "3", 1);
    Used = AddEa(In, Used, &Last, "kmtea1", NULL, 0);
    Status = SetEas(Handle, In, Used);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(Handle);

    /* Persisted across reopen. */
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ | FILE_READ_EA | FILE_WRITE_EA, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Free;
    Status = QueryEas(Handle, Out, FALSE, NULL, 0, TRUE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        FindEa(Out, "SECOND", &Count);
        ok_eq_ulong(Count, 2UL);
        ok(FindEa(Out, "KMTEA1", NULL) == NULL, "deleted EA still present\n");
        Ea = FindEa(Out, "SECOND", NULL);
        ok(Ea != NULL && Ea->EaValueLength == 5 &&
           RtlCompareMemory(Ea->EaName + Ea->EaNameLength + 1, "short", 5) == 5, "replaced value wrong\n");
        Ea = FindEa(Out, "THIRD", NULL);
        ok(Ea != NULL && Ea->EaValueLength == 1, "added EA missing\n");
    }

    /* An inconsistent list (NextEntryOffset past the end) is rejected. */
    Last = 0;
    Used = AddEa(In, 0, &Last, "bad", "x", 1);
    ((PFILE_FULL_EA_INFORMATION)In)->NextEntryOffset = 0x100;
    Status = SetEas(Handle, In, Used);
    ok_eq_hex(Status, STATUS_EA_LIST_INCONSISTENT);

    /* Removing every EA brings the file back to STATUS_NO_EAS_ON_FILE. */
    Last = 0;
    Used = AddEa(In, 0, &Last, "second", NULL, 0);
    Used = AddEa(In, Used, &Last, "third", NULL, 0);
    Status = SetEas(Handle, In, Used);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = QueryEas(Handle, Out, FALSE, NULL, 0, TRUE, NULL);
    ok_eq_hex(Status, STATUS_NO_EAS_ON_FILE);
    ok_eq_ulong(QueryEaSize(Handle), 0UL);
    ZwClose(Handle);

Free:
    NtfsKmtFree(In);
    NtfsKmtFree(Out);
}

static
VOID
TestEaAtCreate(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    PUCHAR In, Out;
    ULONG Used, Last;
    PFILE_FULL_EA_INFORMATION Ea;

    In = NtfsKmtAlloc(EA_BUFFER_SIZE);
    Out = NtfsKmtAlloc(EA_BUFFER_SIZE);
    if (In == NULL || Out == NULL)
        goto Free;

    /* EAs supplied at create time are stored with the new file. */
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"ea_create.txt");
    Last = 0;
    Used = AddEa(In, 0, &Last, "AtCreate", "yes", 3);
    Status = NtfsKmtCreate(&Handle, NULL, Path, GENERIC_READ | GENERIC_WRITE | FILE_READ_EA, 0,
                           FILE_SHARE_READ, FILE_CREATE, FILE_NON_DIRECTORY_FILE, In, Used, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(QueryEaSize(Handle) != 0, "EaSize 0 after create with EaBuffer\n");
        Status = QueryEas(Handle, Out, FALSE, NULL, 0, TRUE, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            Ea = FindEa(Out, "ATCREATE", NULL);
            ok(Ea != NULL && Ea->EaValueLength == 3, "EA from create not returned\n");
        }
        ZwClose(Handle);
    }

Free:
    NtfsKmtFree(In);
    NtfsKmtFree(Out);
}

START_TEST(NtfsEa)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_ea", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    TestEas(Dir);
    TestEaAtCreate(Dir);

    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
