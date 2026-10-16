/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - NTFS volume control and dirty bit
 * PROGRAMMER:      ReactOS Team
 *
 * NtfsVolumeControl: FSCTL_LOCK_VOLUME / FSCTL_UNLOCK_VOLUME /
 * FSCTL_DISMOUNT_VOLUME and remount on the next open (host test
 * test_fsctl_volume_control).
 *
 * NtfsVolumeDirty: the volume mounted writable, FSCTL_IS_VOLUME_DIRTY and
 * FSCTL_MARK_VOLUME_DIRTY. This is the black-box part of the $LogFile work
 * (test_logfile_journaling / test_logfile_recovery / test_lfs_* stay host
 * only: log record layout and replay are not observable through the API).
 * WARNING: FSCTL_MARK_VOLUME_DIRTY persists; nothing but chkdsk clears it,
 * so this entry leaves the data volume dirty. It is the last NTFS entry.
 *
 * Windows reference: the FSCTL documentation, MS-FSA 2.1.5.10.x, FastFAT and
 * NTFS both reject FSCTL_IS_VOLUME_DIRTY on anything but a volume open.
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

START_TEST(NtfsVolumeControl)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    WCHAR Path[MAX_PATH];
    WCHAR Letter;
    NTSTATUS Status;
    HANDLE Volume, File;
    UCHAR Data[16];
    ULONG_PTR Read;

    if (!NtfsKmtFindVolume(Root, &Letter))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }
    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_volctl", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"survivor.txt");
    Status = NtfsKmtCreateFileWithData(Path, "survives dismount", 17);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* Another open handle on the volume blocks the lock. */
    Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = NtfsKmtFsctl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    if (NT_SUCCESS(Status))
        NtfsKmtFsctl(Volume, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    if (File != NULL)
        ZwClose(File);

    /* Unlock without a lock. */
    Status = NtfsKmtFsctl(Volume, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_NOT_LOCKED);

    /* Lock with no other user handle: opens are then refused. */
    Status = NtfsKmtFsctl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
        ok_eq_hex(Status, STATUS_ACCESS_DENIED);
        if (NT_SUCCESS(Status))
            ZwClose(File);
        Status = NtfsKmtFsctl(Volume, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = NtfsKmtFsctl(Volume, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_NOT_LOCKED);
    }

    /* Opens work again after unlock. */
    Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        ZwClose(File);

    /* Lock + dismount; the next open remounts and finds the data. */
    Status = NtfsKmtFsctl(Volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsKmtFsctl(Volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    ZwClose(Volume);

    Status = NtfsKmtOpen(&File, Path, GENERIC_READ, FILE_NON_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(Data, sizeof(Data));
        Status = NtfsKmtRead(File, 0, Data, sizeof(Data), &Read);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(Read == 16 && RtlCompareMemory(Data, "survives dismoun", 16) == 16, "data lost across dismount\n");
        ZwClose(File);
    }

    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

START_TEST(NtfsVolumeDirty)
{
    WCHAR Root[MAX_PATH];
    WCHAR Dir[MAX_PATH];
    WCHAR Letter;
    NTSTATUS Status;
    HANDLE Volume, DirHandle;
    ULONG State;
    ULONG_PTR Info;
    IO_STATUS_BLOCK IoStatus;
    UCHAR AttrBuffer[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 32 * sizeof(WCHAR)];
    PFILE_FS_ATTRIBUTE_INFORMATION Attr = (PFILE_FS_ATTRIBUTE_INFORMATION)AttrBuffer;

    if (!NtfsKmtFindVolume(Root, &Letter))
    {
        ok(FALSE, "No writable NTFS volume found (attach an NTFS data disk)\n");
        return;
    }

    /* The volume mounted read/write: not flagged read-only, writes work. */
    Status = NtfsKmtPrepareDirectory(Root, L"kmt_ntfs_dirty", Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Status = NtfsKmtOpen(&DirHandle, Dir, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    RtlZeroMemory(AttrBuffer, sizeof(AttrBuffer));
    Status = ZwQueryVolumeInformationFile(DirHandle, &IoStatus, Attr, sizeof(AttrBuffer),
                                          FileFsAttributeInformation);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(!(Attr->FileSystemAttributes & FILE_READ_ONLY_VOLUME), "volume mounted read-only\n");

    Status = NtfsKmtOpenVolume(Letter, FILE_READ_DATA | FILE_WRITE_DATA, &Volume);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        ZwClose(DirHandle);
        return;
    }

    /* Only a volume handle can be asked. */
    Status = NtfsKmtFsctl(DirHandle, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &State, sizeof(State), NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ZwClose(DirHandle);

    /* A cleanly formatted, never-corrupted volume is not dirty. */
    State = 0xFFFFFFFF;
    Status = NtfsKmtFsctl(Volume, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &State, sizeof(State), &Info);
    if (skip(!NtfsKmtIsUnimplemented(Status), "FSCTL_IS_VOLUME_DIRTY not supported (0x%lx)\n", Status))
    {
        ZwClose(Volume);
        return;
    }
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Info, sizeof(ULONG));
    ok(!(State & VOLUME_IS_DIRTY), "volume reported dirty before MARK_VOLUME_DIRTY (0x%lx)\n", State);

    /* MARK_VOLUME_DIRTY sets the bit. */
    Status = NtfsKmtFsctl(Volume, FSCTL_MARK_VOLUME_DIRTY, NULL, 0, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    State = 0;
    Status = NtfsKmtFsctl(Volume, FSCTL_IS_VOLUME_DIRTY, NULL, 0, &State, sizeof(State), &Info);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(State & VOLUME_IS_DIRTY, "VOLUME_IS_DIRTY not set after MARK_VOLUME_DIRTY (0x%lx)\n", State);

    /* A dirty volume stays writable while mounted. */
    {
        WCHAR Path[MAX_PATH];
        NtfsKmtJoin(Path, RTL_NUMBER_OF(Path), Dir, L"after_dirty.txt");
        Status = NtfsKmtCreateFileWithData(Path, "still writable", 14);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    ZwClose(Volume);
    Status = NtfsKmtDeleteTree(Dir);
    ok_eq_hex(Status, STATUS_SUCCESS);
}
