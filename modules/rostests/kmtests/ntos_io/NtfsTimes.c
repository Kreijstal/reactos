/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     NTFS per-open timestamp policy (MS-FSA 2.1.5.15.2)
 */
#include <kmt_test.h>
#include "NtfsKmt.h"

#define OLD_TIME 125000000000000000LL

static NTSTATUS
SetBasic(HANDLE File, LONGLONG WriteTime, LONGLONG ChangeTime, ULONG Attributes)
{
    FILE_BASIC_INFORMATION Basic;
    IO_STATUS_BLOCK IoStatus;

    RtlZeroMemory(&Basic, sizeof(Basic));
    Basic.LastWriteTime.QuadPart = WriteTime;
    Basic.ChangeTime.QuadPart = ChangeTime;
    Basic.FileAttributes = Attributes;
    return ZwSetInformationFile(File, &IoStatus, &Basic, sizeof(Basic), FileBasicInformation);
}

static VOID
CheckTimes(PCWSTR Path, BOOLEAN Changed)
{
    HANDLE File;
    FILE_BASIC_INFORMATION Basic;
    FILE_NETWORK_OPEN_INFORMATION Network;
    NTSTATUS Status;

    Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = NtfsKmtQueryInfo(File, &Basic, sizeof(Basic), FileBasicInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        if (Changed)
        {
            ok(Basic.LastWriteTime.QuadPart > OLD_TIME, "write time did not resume\n");
            ok(Basic.ChangeTime.QuadPart > OLD_TIME, "change time did not resume\n");
        }
        else
        {
            ok_eq_longlong(Basic.LastWriteTime.QuadPart, OLD_TIME);
            ok_eq_longlong(Basic.ChangeTime.QuadPart, OLD_TIME);
        }
        ok(Basic.FileAttributes & FILE_ATTRIBUTE_ARCHIVE, "write did not set ARCHIVE\n");
        Status = NtfsKmtQueryInfo(File, &Network, sizeof(Network), FileNetworkOpenInformation, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok_eq_longlong(Network.LastWriteTime.QuadPart, Basic.LastWriteTime.QuadPart);
            ok_eq_longlong(Network.ChangeTime.QuadPart, Basic.ChangeTime.QuadPart);
        }
    }
    ZwClose(File);
}

static VOID
ResetTimes(PCWSTR Path)
{
    HANDLE File;
    NTSTATUS Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Status = SetBasic(File, OLD_TIME, OLD_TIME, FILE_ATTRIBUTE_NORMAL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(File);
}

static VOID
TestTimes(PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE File = NULL, Other = NULL;
    FILE_BASIC_INFORMATION Basic;
    NTSTATUS Status;
    ULONG i;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"times.bin");
    Status = NtfsKmtCreateFileWithData(Path, "x", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;

    for (i = 0; i < 3; i++)
    {
        ResetTimes(Path);
        Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) goto Cleanup;
        /* Explicit values pin just as -1 does. -2 resumes both policies. */
        Status = SetBasic(File, i == 1 ? OLD_TIME : -1, i == 1 ? OLD_TIME : -1, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (i == 2)
        {
            Status = SetBasic(File, -2, -2, 0);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
        Status = NtfsKmtWrite(File, 0, "y", 1, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(File); File = NULL;
        CheckTimes(Path, i == 2);
    }

    /* A second open must not inherit the first open's suppression state. */
    ResetTimes(Path);
    Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = SetBasic(File, -1, -1, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&Other, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtWrite(Other, 0, "z", 1, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(Other); Other = NULL;
    Status = NtfsKmtQueryInfo(File, &Basic, sizeof(Basic), FileBasicInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok(Basic.LastWriteTime.QuadPart > OLD_TIME, "pinned open hides another open's write time\n");
        ok(Basic.ChangeTime.QuadPart > OLD_TIME, "pinned open hides another open's change time\n");
    }
    {
        PIRP Previous = IoGetTopLevelIrp();
        PIRP Sentinel = (PIRP)FSRTL_FSP_TOP_LEVEL_IRP;
        PIRP After;

        IoSetTopLevelIrp(Sentinel);
        Status = NtfsKmtQueryInfo(File, &Basic, sizeof(Basic), FileBasicInformation, NULL);
        After = IoGetTopLevelIrp();
        IoSetTopLevelIrp(Previous);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(After == Sentinel, "nested dispatch cleared the caller's top-level IRP\n");
    }
    Status = SetBasic(File, -3, 0, 0);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = SetBasic(File, 0, -3, 0);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = SetBasic(File, 0, 0, FILE_ATTRIBUTE_DIRECTORY);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ZwClose(File); File = NULL;

    /* A zero-byte write must not schedule a cleanup timestamp update. */
    ResetTimes(Path);
    Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtWrite(File, 0, "", 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(File); File = NULL;
    Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Status = NtfsKmtQueryInfo(File, &Basic, sizeof(Basic), FileBasicInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_longlong(Basic.LastWriteTime.QuadPart, OLD_TIME);
        ok_eq_longlong(Basic.ChangeTime.QuadPart, OLD_TIME);
    }
Cleanup:
    if (Other != NULL) ZwClose(Other);
    if (File != NULL) ZwClose(File);
}

static VOID
TestSizeTimes(PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    FILE_END_OF_FILE_INFORMATION End;
    FILE_ALLOCATION_INFORMATION Allocation;
    FILE_BASIC_INFORMATION Basic;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;
    ULONG i;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"size-times.bin");
    Status = NtfsKmtCreateFileWithData(Path, "a", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    for (i = 0; i < 3; i++)
    {
        ResetTimes(Path);
        Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) return;
        if (i < 2)
        {
            /* Even setting the existing EOF notes a modification. */
            End.EndOfFile.QuadPart = i == 0 ? 1 : 2;
            Status = ZwSetInformationFile(File, &IoStatus, &End, sizeof(End), FileEndOfFileInformation);
        }
        else
        {
            Allocation.AllocationSize.QuadPart = 65536;
            Status = ZwSetInformationFile(File, &IoStatus, &Allocation, sizeof(Allocation), FileAllocationInformation);
        }
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(File);
        Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) return;
        Status = NtfsKmtQueryInfo(File, &Basic, sizeof(Basic), FileBasicInformation, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            if (i < 2)
                ok(Basic.LastWriteTime.QuadPart > OLD_TIME, "EOF did not update write time\n");
            else
                ok_eq_longlong(Basic.LastWriteTime.QuadPart, OLD_TIME);
            ok(Basic.ChangeTime.QuadPart > OLD_TIME, "size operation did not update change time\n");
            ok(Basic.FileAttributes & FILE_ATTRIBUTE_ARCHIVE, "size operation did not set ARCHIVE\n");
        }
        ZwClose(File);
    }
}

static VOID
TestStoredAttributes(PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    FILE_BASIC_INFORMATION Basic;
    USHORT Compression = COMPRESSION_FORMAT_LZNT1;
    NTSTATUS Status;
    ULONG i;

    for (i = 0; i < 2; i++)
    {
        NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, i == 0 ? L"sparse.bin" : L"compressed.bin");
        Status = NtfsKmtCreateFileWithData(Path, "a", 1);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) continue;
        Status = NtfsKmtOpen(&File, Path, FILE_ALL_ACCESS, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status)) continue;
        if (i == 0)
            Status = NtfsKmtFsctl(File, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, NULL);
        else
            Status = NtfsKmtFsctl(File, FSCTL_SET_COMPRESSION, &Compression, sizeof(Compression), NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = SetBasic(File, 0, 0, FILE_ATTRIBUTE_NORMAL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = NtfsKmtQueryInfo(File, &Basic, sizeof(Basic), FileBasicInformation, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
            ok(Basic.FileAttributes & (i == 0 ? FILE_ATTRIBUTE_SPARSE_FILE : FILE_ATTRIBUTE_COMPRESSED),
               "basic attributes removed stored stream state: %lx\n", Basic.FileAttributes);
        ZwClose(File);
    }
}

START_TEST(NtfsTimes)
{
    WCHAR Root[MAX_PATH], Dir[MAX_PATH];
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL)) return;
    Status = NtfsKmtPrepareDirectory(Root, L"KMT_NTFS_TIMES", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    TestTimes(Dir);
    TestStoredAttributes(Dir);
    TestSizeTimes(Dir);
    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
