/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS reparse points
 * PROGRAMMER:      ReactOS Team
 *
 * Black-box reparse-point coverage: FSCTL_SET/GET/DELETE_REPARSE_POINT with a
 * mount point (IO_REPARSE_TAG_MOUNT_POINT on a directory, then resolved by a
 * path walk through it) and with a third-party tag (REPARSE_GUID_DATA_BUFFER
 * on a file), FileAttributeTagInformation, FILE_ATTRIBUTE_REPARSE_POINT, the
 * buffer-size statuses, tag mismatch and non-empty-directory rejection.
 *
 * Replaces the reparse half of the host tests test_ntfs_metadata_roundtrip
 * and test_ntfs_metadata_features.
 *
 * Windows reference: FSCTL_SET_REPARSE_POINT / FSCTL_GET_REPARSE_POINT /
 * FSCTL_DELETE_REPARSE_POINT docs, MS-FSA 2.1.5.10.x, MS-FSCC 2.1.2,
 * Wine ntdll/file.c test_reparse_points.
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

/* A non-Microsoft tag: high bit clear, so it needs a GUID buffer. */
#define TEST_TAG        0x0000ABCDUL

static const GUID TestGuid =
    { 0x6b29fc40, 0xca47, 0x1067, { 0xb3, 0x1d, 0x00, 0xdd, 0x01, 0x06, 0x62, 0xda } };

static
PREPARSE_DATA_BUFFER
BuildMountPoint(
    _In_ PCWSTR Target,
    _Out_ PULONG Size)
{
    PREPARSE_DATA_BUFFER Buffer;
    USHORT SubstLen = (USHORT)(wcslen(Target) * sizeof(WCHAR));
    USHORT PrintLen = SubstLen;
    USHORT DataLen;

    /* PathBuffer: substitute name, NUL, print name, NUL. */
    DataLen = (USHORT)(FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer) -
                       REPARSE_DATA_BUFFER_HEADER_SIZE + SubstLen + PrintLen + 2 * sizeof(WCHAR));
    *Size = REPARSE_DATA_BUFFER_HEADER_SIZE + DataLen;
    Buffer = NtfsKmtAlloc(*Size);
    if (Buffer == NULL)
        return NULL;
    Buffer->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    Buffer->ReparseDataLength = DataLen;
    Buffer->MountPointReparseBuffer.SubstituteNameOffset = 0;
    Buffer->MountPointReparseBuffer.SubstituteNameLength = SubstLen;
    Buffer->MountPointReparseBuffer.PrintNameOffset = SubstLen + sizeof(WCHAR);
    Buffer->MountPointReparseBuffer.PrintNameLength = PrintLen;
    RtlCopyMemory(Buffer->MountPointReparseBuffer.PathBuffer, Target, SubstLen);
    RtlCopyMemory((PUCHAR)Buffer->MountPointReparseBuffer.PathBuffer + SubstLen + sizeof(WCHAR),
                  Target, PrintLen);
    return Buffer;
}

static
ULONG
QueryTag(
    _In_ PCWSTR Path,
    _Out_opt_ PULONG Attributes)
{
    HANDLE Handle;
    FILE_ATTRIBUTE_TAG_INFORMATION Tag;
    NTSTATUS Status;

    if (Attributes != NULL)
        *Attributes = 0;
    Status = NtfsKmtOpen(&Handle, Path, FILE_READ_ATTRIBUTES, FILE_OPEN_REPARSE_POINT);
    if (!NT_SUCCESS(Status))
        return 0xFFFFFFFF;
    RtlZeroMemory(&Tag, sizeof(Tag));
    Status = NtfsKmtQueryInfo(Handle, &Tag, sizeof(Tag), FileAttributeTagInformation, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ZwClose(Handle);
    if (Attributes != NULL)
        *Attributes = Tag.FileAttributes;
    return Tag.ReparseTag;
}

static
VOID
TestNotAReparsePoint(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    UCHAR Small[1];
    UCHAR Buffer[64];
    ULONG Attributes;

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"plain.txt");
    Status = NtfsKmtCreateFileWithData(Path, "x", 1);
    ok_eq_hex(Status, STATUS_SUCCESS);

    ok_eq_hex(QueryTag(Path, &Attributes), 0UL);
    ok(!(Attributes & FILE_ATTRIBUTE_REPARSE_POINT), "plain file has FILE_ATTRIBUTE_REPARSE_POINT\n");

    Status = NtfsKmtOpen(&Handle, Path, FILE_READ_ATTRIBUTES, FILE_OPEN_REPARSE_POINT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    /* Wine test_reparse_points: no buffer / zero-length buffer is
     * STATUS_INVALID_USER_BUFFER, a 1-byte buffer reaches the file system
     * which reports STATUS_NOT_A_REPARSE_POINT. */
    Status = NtfsKmtFsctl(Handle, FSCTL_GET_REPARSE_POINT, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_INVALID_USER_BUFFER);
    Status = NtfsKmtFsctl(Handle, FSCTL_GET_REPARSE_POINT, NULL, 0, Small, 0, NULL);
    ok_eq_hex(Status, STATUS_INVALID_USER_BUFFER);
    Status = NtfsKmtFsctl(Handle, FSCTL_GET_REPARSE_POINT, NULL, 0, Small, sizeof(Small), NULL);
    if (!skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_GET_REPARSE_POINT not supported (0x%lx)\n", Status))
        ok_eq_hex(Status, STATUS_NOT_A_REPARSE_POINT);
    Status = NtfsKmtFsctl(Handle, FSCTL_GET_REPARSE_POINT, NULL, 0, Buffer, sizeof(Buffer), NULL);
    ok_eq_hex(Status, STATUS_NOT_A_REPARSE_POINT);
    ZwClose(Handle);
}

static
VOID
TestMountPoint(
    _In_ PCWSTR Dir)
{
    WCHAR Target[MAX_PATH];
    WCHAR Mount[MAX_PATH];
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    PREPARSE_DATA_BUFFER Reparse, Got;
    ULONG Size, Attributes;
    ULONG_PTR Info;
    UCHAR Data[16];
    ULONG_PTR Read;

    NtfsKmtJoin(Target, RTL_NUMBER_OF(Target), Dir, L"target");
    NtfsKmtJoin(Mount, RTL_NUMBER_OF(Mount), Dir, L"mount");
    Status = NtfsKmtCreateDirectory(Target);
    ok_eq_hex(Status, STATUS_SUCCESS);
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Target, L"inside.txt");
    Status = NtfsKmtCreateFileWithData(Path, "through mp", 10);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtCreateDirectory(Mount);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Substitute name: the NT path of the target with a trailing '\'. */
    RtlStringCchCatW(Target, RTL_NUMBER_OF(Target), L"\\");
    Reparse = BuildMountPoint(Target, &Size);
    if (Reparse == NULL)
        return;

    Status = NtfsKmtOpen(&Handle, Mount, FILE_WRITE_DATA | FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                         FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        NtfsKmtFree(Reparse);
        return;
    }

    /* A length that disagrees with ReparseDataLength is invalid. */
    Status = NtfsKmtFsctl(Handle, FSCTL_SET_REPARSE_POINT, Reparse, Size - 2, NULL, 0, NULL);
    if (skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_SET_REPARSE_POINT not supported (0x%lx)\n", Status))
    {
        ZwClose(Handle);
        NtfsKmtFree(Reparse);
        return;
    }
    ok_eq_hex(Status, STATUS_IO_REPARSE_DATA_INVALID);

    Status = NtfsKmtFsctl(Handle, FSCTL_SET_REPARSE_POINT, Reparse, Size, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* GET returns the same bytes; a header-only buffer overflows, a buffer
     * smaller than the header is too small. */
    Got = NtfsKmtAlloc(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    if (Got != NULL)
    {
        Status = NtfsKmtFsctl(Handle, FSCTL_GET_REPARSE_POINT, NULL, 0, Got,
                              MAXIMUM_REPARSE_DATA_BUFFER_SIZE, &Info);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulongptr(Info, Size);
        ok(RtlCompareMemory(Got, Reparse, Size) == Size, "GET_REPARSE_POINT data differs\n");

        RtlZeroMemory(Got, 64);
        Status = NtfsKmtFsctl(Handle, FSCTL_GET_REPARSE_POINT, NULL, 0, Got,
                              REPARSE_DATA_BUFFER_HEADER_SIZE, &Info);
        ok_eq_hex(Status, STATUS_BUFFER_OVERFLOW);
        ok_eq_ulongptr(Info, REPARSE_DATA_BUFFER_HEADER_SIZE);
        ok_eq_hex(Got->ReparseTag, IO_REPARSE_TAG_MOUNT_POINT);
        ok_eq_uint(Got->ReparseDataLength, Reparse->ReparseDataLength);

        Status = NtfsKmtFsctl(Handle, FSCTL_GET_REPARSE_POINT, NULL, 0, Got,
                              REPARSE_DATA_BUFFER_HEADER_SIZE - 1, &Info);
        ok_eq_hex(Status, STATUS_BUFFER_TOO_SMALL);
        NtfsKmtFree(Got);
    }
    ZwClose(Handle);

    ok_eq_hex(QueryTag(Mount, &Attributes), IO_REPARSE_TAG_MOUNT_POINT);
    ok(Attributes & FILE_ATTRIBUTE_REPARSE_POINT, "mount point lacks FILE_ATTRIBUTE_REPARSE_POINT\n");
    ok(Attributes & FILE_ATTRIBUTE_DIRECTORY, "mount point lacks FILE_ATTRIBUTE_DIRECTORY\n");

    /* A path walk through the mount point lands in the target. */
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Mount, L"inside.txt");
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(Data, sizeof(Data));
        Status = NtfsKmtRead(Handle, 0, Data, sizeof(Data), &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(Read == 10 && RtlCompareMemory(Data, "through mp", 10) == 10, "read through mount point mismatch\n");
        ZwClose(Handle);
    }

    /* Directory enumeration reports the tag in EaSize of a reparse entry. */
    {
        HANDLE DirHandle;
        IO_STATUS_BLOCK IoStatus;
        UNICODE_STRING Mask = RTL_CONSTANT_STRING(L"mount");
        PFILE_FULL_DIR_INFORMATION Full = NtfsKmtAlloc(1024);

        if (Full != NULL &&
            NT_SUCCESS(NtfsKmtOpen(&DirHandle, Dir, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE)))
        {
            Status = ZwQueryDirectoryFile(DirHandle, NULL, NULL, NULL, &IoStatus, Full, 1024,
                                          FileFullDirectoryInformation, TRUE, &Mask, TRUE);
            ok_eq_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
            {
                ok(Full->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT,
                   "directory entry lacks FILE_ATTRIBUTE_REPARSE_POINT\n");
                ok_eq_hex(Full->EaSize, IO_REPARSE_TAG_MOUNT_POINT);
            }
            ZwClose(DirHandle);
        }
        NtfsKmtFree(Full);
    }

    /* Delete with the wrong tag fails, with the right tag succeeds. */
    Status = NtfsKmtOpen(&Handle, Mount, FILE_WRITE_DATA | FILE_READ_ATTRIBUTES,
                         FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        REPARSE_DATA_BUFFER Header;
        REPARSE_GUID_DATA_BUFFER GuidHeader;

        RtlZeroMemory(&Header, sizeof(Header));
        Header.ReparseTag = IO_REPARSE_TAG_SYMLINK;
        Status = NtfsKmtFsctl(Handle, FSCTL_DELETE_REPARSE_POINT, &Header,
                              REPARSE_DATA_BUFFER_HEADER_SIZE, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_IO_REPARSE_TAG_MISMATCH);
        if (NT_SUCCESS(Status))
        {
            /* It wrongly went away: put it back so the next check stands alone. */
            NtfsKmtFsctl(Handle, FSCTL_SET_REPARSE_POINT, Reparse, Size, NULL, 0, NULL);
        }

        /* Windows also takes the GUID header for a Microsoft tag (Wine's
         * test_reparse_points deletes its junction that way). */
        RtlZeroMemory(&GuidHeader, sizeof(GuidHeader));
        GuidHeader.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
        Status = NtfsKmtFsctl(Handle, FSCTL_DELETE_REPARSE_POINT, &GuidHeader,
                              REPARSE_GUID_DATA_BUFFER_HEADER_SIZE, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Handle);
    }
    ok_eq_hex(QueryTag(Mount, &Attributes), 0UL);
    ok(!(Attributes & FILE_ATTRIBUTE_REPARSE_POINT), "FILE_ATTRIBUTE_REPARSE_POINT left after delete\n");

    /* Now an ordinary empty directory: the old walk no longer resolves. */
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Mount, L"inside.txt");
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_NOT_FOUND);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);

    /* A mount point cannot be set on a non-empty directory. */
    Status = NtfsKmtOpen(&Handle, Dir, FILE_WRITE_DATA | FILE_READ_ATTRIBUTES,
                         FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtFsctl(Handle, FSCTL_SET_REPARSE_POINT, Reparse, Size, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_DIRECTORY_NOT_EMPTY);
        if (NT_SUCCESS(Status))
        {
            REPARSE_DATA_BUFFER Header;
            RtlZeroMemory(&Header, sizeof(Header));
            Header.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
            NtfsKmtFsctl(Handle, FSCTL_DELETE_REPARSE_POINT, &Header,
                         REPARSE_DATA_BUFFER_HEADER_SIZE, NULL, 0, NULL);
        }
        ZwClose(Handle);
    }
    NtfsKmtFree(Reparse);
}

static
VOID
TestCustomTag(
    _In_ PCWSTR Dir)
{
    WCHAR Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    PREPARSE_GUID_DATA_BUFFER Reparse, Got;
    ULONG Size, Attributes;
    ULONG_PTR Info;
    static const UCHAR Payload[] = "third-party reparse payload";

    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"custom.dat");
    Status = NtfsKmtCreateFileWithData(Path, "file data", 9);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Size = REPARSE_GUID_DATA_BUFFER_HEADER_SIZE + sizeof(Payload);
    Reparse = NtfsKmtAlloc(Size);
    Got = NtfsKmtAlloc(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    if (Reparse == NULL || Got == NULL)
        goto Cleanup;
    Reparse->ReparseTag = TEST_TAG;
    Reparse->ReparseDataLength = sizeof(Payload);
    Reparse->ReparseGuid = TestGuid;
    RtlCopyMemory(Reparse->GenericReparseBuffer.DataBuffer, Payload, sizeof(Payload));

    Status = NtfsKmtOpen(&Handle, Path, FILE_WRITE_DATA | FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                         FILE_OPEN_REPARSE_POINT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    /* Reserved tags are invalid. */
    {
        REPARSE_GUID_DATA_BUFFER Bad = *Reparse;
        Bad.ReparseTag = IO_REPARSE_TAG_RESERVED_ONE;
        Bad.ReparseDataLength = 0;
        Status = NtfsKmtFsctl(Handle, FSCTL_SET_REPARSE_POINT, &Bad,
                              REPARSE_GUID_DATA_BUFFER_HEADER_SIZE, NULL, 0, NULL);
        if (skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_SET_REPARSE_POINT not supported (0x%lx)\n", Status))
        {
            ZwClose(Handle);
            goto Cleanup;
        }
        ok_eq_hex(Status, STATUS_IO_REPARSE_TAG_INVALID);
    }

    Status = NtfsKmtFsctl(Handle, FSCTL_SET_REPARSE_POINT, Reparse, Size, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = NtfsKmtFsctl(Handle, FSCTL_GET_REPARSE_POINT, NULL, 0, Got,
                          MAXIMUM_REPARSE_DATA_BUFFER_SIZE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, Size);
    ok(RtlCompareMemory(Got, Reparse, Size) == Size, "custom reparse data differs\n");

    /* Setting a different tag over an existing one is a mismatch. */
    {
        PREPARSE_GUID_DATA_BUFFER Other = NtfsKmtAlloc(Size);
        if (Other != NULL)
        {
            RtlCopyMemory(Other, Reparse, Size);
            Other->ReparseTag = TEST_TAG + 1;
            Status = NtfsKmtFsctl(Handle, FSCTL_SET_REPARSE_POINT, Other, Size, NULL, 0, NULL);
            ok_eq_hex(Status, STATUS_IO_REPARSE_TAG_MISMATCH);
            NtfsKmtFree(Other);
        }
    }

    /* Re-setting the same tag replaces the data. */
    Reparse->GenericReparseBuffer.DataBuffer[0] = 'T';
    Status = NtfsKmtFsctl(Handle, FSCTL_SET_REPARSE_POINT, Reparse, Size, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtFsctl(Handle, FSCTL_GET_REPARSE_POINT, NULL, 0, Got,
                          MAXIMUM_REPARSE_DATA_BUFFER_SIZE, &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Got->GenericReparseBuffer.DataBuffer[0] == 'T', "reparse data not replaced\n");
    ZwClose(Handle);

    ok_eq_hex(QueryTag(Path, &Attributes), TEST_TAG);
    ok(Attributes & FILE_ATTRIBUTE_REPARSE_POINT, "custom reparse lacks FILE_ATTRIBUTE_REPARSE_POINT\n");

    /* Without FILE_OPEN_REPARSE_POINT nobody handles this tag. */
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_IO_REPARSE_TAG_NOT_HANDLED);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);

    /* The unnamed data stream is untouched by the reparse point. */
    Status = NtfsKmtOpen(&Handle, Path, GENERIC_READ | FILE_WRITE_DATA, FILE_OPEN_REPARSE_POINT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        UCHAR Data[16];
        ULONG_PTR Read;
        REPARSE_GUID_DATA_BUFFER Header;

        Status = NtfsKmtRead(Handle, 0, Data, sizeof(Data), &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(Read == 9 && RtlCompareMemory(Data, "file data", 9) == 9, "file data changed\n");

        /* Delete needs the matching GUID for a non-Microsoft tag. */
        RtlZeroMemory(&Header, sizeof(Header));
        Header.ReparseTag = TEST_TAG;
        Status = NtfsKmtFsctl(Handle, FSCTL_DELETE_REPARSE_POINT, &Header,
                              REPARSE_GUID_DATA_BUFFER_HEADER_SIZE, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_REPARSE_ATTRIBUTE_CONFLICT);
        if (NT_SUCCESS(Status))
            NtfsKmtFsctl(Handle, FSCTL_SET_REPARSE_POINT, Reparse, Size, NULL, 0, NULL);
        Header.ReparseGuid = TestGuid;
        Status = NtfsKmtFsctl(Handle, FSCTL_DELETE_REPARSE_POINT, &Header,
                              REPARSE_GUID_DATA_BUFFER_HEADER_SIZE, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Handle);
    }
    ok_eq_hex(QueryTag(Path, &Attributes), 0UL);

Cleanup:
    NtfsKmtFree(Reparse);
    NtfsKmtFree(Got);
}

START_TEST(NtfsReparse)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    NTSTATUS Status;

    if (!NtfsKmtFindVolume(Root, NULL))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_reparse", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    TestNotAReparsePoint(Dir);
    TestMountPoint(Dir);
    TestCustomTag(Dir);

    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
