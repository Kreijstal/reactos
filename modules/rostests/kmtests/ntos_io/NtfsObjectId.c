/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS object IDs
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box object-ID coverage: FSCTL_GET/SET/CREATE_OR_GET/DELETE_OBJECT_ID,
 * the extended-info round trip, duplicate-ID rejection across files, and
 * open by object ID (FILE_OPEN_BY_FILE_ID with a 16-byte ID relative to a
 * volume handle).
 *
 * Replaces the object-ID half of the host tests test_ntfs_metadata_roundtrip
 * and test_ntfs_metadata_features.
 *
 * Windows reference: FSCTL_*_OBJECT_ID docs, MS-FSA 2.1.5.10.x (a file
 * without an ID: STATUS_OBJECTID_NOT_FOUND),
 * MS-FSCC 2.1.3 FILE_OBJECTID_BUFFER.
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

static
VOID
MakeId(
    _Out_ PFILE_OBJECTID_BUFFER Buffer,
    _In_ UCHAR Seed)
{
    ULONG i;

    RtlZeroMemory(Buffer, sizeof(*Buffer));
    for (i = 0; i < 16; i++)
    {
        Buffer->ObjectId[i] = (UCHAR)(0xA0 + Seed + i * 7);
        Buffer->BirthVolumeId[i] = (UCHAR)(0x10 + i);
        Buffer->BirthObjectId[i] = (UCHAR)(0xB0 + Seed + i);
        Buffer->DomainId[i] = 0;
    }
}

static
NTSTATUS
GetId(
    _In_ HANDLE Handle,
    _Out_ PFILE_OBJECTID_BUFFER Buffer,
    _Out_opt_ PULONG_PTR Info)
{
    RtlZeroMemory(Buffer, sizeof(*Buffer));
    return NtfsKmtFsctl(Handle, FSCTL_GET_OBJECT_ID, NULL, 0, Buffer, sizeof(*Buffer), Info);
}

static
BOOLEAN
IsZero16(
    _In_reads_bytes_(16) const UCHAR *Bytes)
{
    return NtfsKmtIsZero(Bytes, 16);
}

static
VOID
TestObjectIds(
    _In_ PCWSTR Dir,
    _In_ WCHAR Letter)
{
    WCHAR PathA[MAX_PATH], PathB[MAX_PATH], PathC[MAX_PATH];
    HANDLE A, B, C;
    NTSTATUS Status;
    FILE_OBJECTID_BUFFER Set, Got, Got2;
    ULONG_PTR Info;

    NtfsKmtJoin(PathA, RTL_NUMBER_OF(PathA), Dir, L"oid_a.txt");
    NtfsKmtJoin(PathB, RTL_NUMBER_OF(PathB), Dir, L"oid_b.txt");
    NtfsKmtJoin(PathC, RTL_NUMBER_OF(PathC), Dir, L"oid_c.txt");
    Status = NtfsKmtCreateFileWithData(PathA, "A", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateFileWithData(PathB, "B", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateFileWithData(PathC, "C", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = NtfsKmtOpen(&A, PathA, GENERIC_READ | GENERIC_WRITE, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&B, PathB, GENERIC_READ | GENERIC_WRITE, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtOpen(&C, PathC, GENERIC_READ | GENERIC_WRITE, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (A == NULL || B == NULL || C == NULL)
        goto Close;

    /* A file without an object ID. */
    Status = GetId(A, &Got, NULL);
    if (skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_GET_OBJECT_ID not supported (0x%lx)\n", Status))
        goto Close;
    ok_eq_hex(Status, STATUS_OBJECTID_NOT_FOUND);

    /* SET then GET round-trips all 64 bytes. */
    MakeId(&Set, 1);
    Status = NtfsKmtFsctl(A, FSCTL_SET_OBJECT_ID, &Set, sizeof(Set), NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = GetId(A, &Got, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, sizeof(FILE_OBJECTID_BUFFER));
    ok(RtlCompareMemory(Got.ObjectId, Set.ObjectId, 16) == 16, "ObjectId round trip differs\n");
    ok(RtlCompareMemory(Got.ExtendedInfo, Set.ExtendedInfo, 48) == 48,
       "extended info (birth volume/object, domain) not stored\n");

    /* A second SET on a file that has one collides. */
    MakeId(&Got2, 2);
    Status = NtfsKmtFsctl(A, FSCTL_SET_OBJECT_ID, &Got2, sizeof(Got2), NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_COLLISION);

    /* The same object ID cannot be given to a second file. */
    Status = NtfsKmtFsctl(B, FSCTL_SET_OBJECT_ID, &Set, sizeof(Set), NULL, 0, NULL);
    ok(Status == STATUS_DUPLICATE_NAME || Status == STATUS_DUPLICATE_OBJECTID,
       "duplicate object ID on a second file: 0x%lx\n", Status);

    /* CREATE_OR_GET on a file that has one returns it unchanged. */
    Status = NtfsKmtFsctl(A, FSCTL_CREATE_OR_GET_OBJECT_ID, NULL, 0, &Got, sizeof(Got), &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, sizeof(FILE_OBJECTID_BUFFER));
    ok(RtlCompareMemory(Got.ObjectId, Set.ObjectId, 16) == 16, "CREATE_OR_GET changed an existing ID\n");

    /* CREATE_OR_GET on a file without one makes a fresh, stable ID. */
    Status = NtfsKmtFsctl(C, FSCTL_CREATE_OR_GET_OBJECT_ID, NULL, 0, &Got, sizeof(Got), &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, sizeof(FILE_OBJECTID_BUFFER));
    ok(!IsZero16(Got.ObjectId), "CREATE_OR_GET made an all-zero ID\n");
    ok(RtlCompareMemory(Got.ObjectId, Set.ObjectId, 16) != 16, "CREATE_OR_GET reused another file's ID\n");
    ok(RtlCompareMemory(Got.BirthObjectId, Got.ObjectId, 16) == 16, "generated BirthObjectId != ObjectId\n");
    Status = NtfsKmtFsctl(C, FSCTL_CREATE_OR_GET_OBJECT_ID, NULL, 0, &Got2, sizeof(Got2), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(RtlCompareMemory(Got2.ObjectId, Got.ObjectId, 16) == 16, "CREATE_OR_GET not stable\n");
    Status = GetId(C, &Got2, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(RtlCompareMemory(Got2.ObjectId, Got.ObjectId, 16) == 16, "GET disagrees with CREATE_OR_GET\n");

    /* Open by object ID: 16-byte ID relative to a volume handle. */
    {
        HANDLE Volume;
        HANDLE ById;
        UNICODE_STRING Name;
        OBJECT_ATTRIBUTES ObjectAttributes;
        IO_STATUS_BLOCK IoStatus;

        Status = NtfsKmtOpenVolume(Letter, FILE_READ_ATTRIBUTES, &Volume);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            Name.Buffer = (PWSTR)Set.ObjectId;
            Name.Length = Name.MaximumLength = 16;
            InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_KERNEL_HANDLE, Volume, NULL);
            Status = ZwCreateFile(&ById, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &ObjectAttributes,
                                  &IoStatus, NULL, 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  FILE_OPEN, FILE_OPEN_BY_FILE_ID | FILE_SYNCHRONOUS_IO_NONALERT,
                                  NULL, 0);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
            {
                ok(NtfsKmtQueryFileId(ById) == NtfsKmtQueryFileId(A), "open by object ID reached another file\n");
                ZwClose(ById);
            }
            ZwClose(Volume);
        }
    }

    /* DELETE removes it; the ID is then free for another file. */
    Status = NtfsKmtFsctl(A, FSCTL_DELETE_OBJECT_ID, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = GetId(A, &Got, NULL);
    ok_eq_hex(Status, STATUS_OBJECTID_NOT_FOUND);
    Status = NtfsKmtFsctl(B, FSCTL_SET_OBJECT_ID, &Set, sizeof(Set), NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = GetId(B, &Got, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(RtlCompareMemory(Got.ObjectId, Set.ObjectId, 16) == 16, "reused ID not stored on B\n");

    /* The ID survives close/reopen. */
    ZwClose(B);
    B = NULL;
    Status = NtfsKmtOpen(&B, PathB, GENERIC_READ, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = GetId(B, &Got, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(RtlCompareMemory(Got.ObjectId, Set.ObjectId, 16) == 16, "ObjectId lost on reopen\n");
        ok(RtlCompareMemory(Got.ExtendedInfo, Set.ExtendedInfo, 48) == 48, "extended info lost on reopen\n");
    }

    /* Deleting the file releases its ID (the $O index entry goes too). */
    ZwClose(B);
    B = NULL;
    Status = NtfsKmtDelete(PathB);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtFsctl(A, FSCTL_SET_OBJECT_ID, &Set, sizeof(Set), NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtFsctl(A, FSCTL_DELETE_OBJECT_ID, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtFsctl(C, FSCTL_DELETE_OBJECT_ID, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

Close:
    if (A != NULL) ZwClose(A);
    if (B != NULL) ZwClose(B);
    if (C != NULL) ZwClose(C);
}

START_TEST(NtfsObjectId)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    WCHAR Letter;
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, &Letter))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_objid", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    TestObjectIds(Dir, Letter);

    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
