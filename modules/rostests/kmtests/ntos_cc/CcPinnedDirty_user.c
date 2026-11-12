/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite: pinned data modified after
 *                  CcSetDirtyPinnedData reaches the disk (user-mode part)
 * PROGRAMMER:      ReactOS Developers
 */

#include <kmt_test.h>

START_TEST(CcPinnedDirty)
{
    HANDLE Handle;
    NTSTATUS Status;
    LARGE_INTEGER ByteOffset;
    IO_STATUS_BLOCK IoStatusBlock;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING File = RTL_CONSTANT_STRING(L"\\Device\\Kmtest-CcPinnedDirty\\TestFile");
    UCHAR Buffer[512] = { 0 };
    DWORD Error;

    Error = KmtLoadAndOpenDriver(L"CcPinnedDirty", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    InitializeObjectAttributes(&ObjectAttributes, &File, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Handle, FILE_ALL_ACCESS, &ObjectAttributes, &IoStatusBlock, 0,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        /* The cached write runs the whole test in the driver. */
        ByteOffset.QuadPart = 0;
        Status = NtWriteFile(Handle, NULL, NULL, NULL, &IoStatusBlock, Buffer, sizeof(Buffer), &ByteOffset, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        NtClose(Handle);
    }

    KmtCloseDriver();
    KmtUnloadDriver();
}
