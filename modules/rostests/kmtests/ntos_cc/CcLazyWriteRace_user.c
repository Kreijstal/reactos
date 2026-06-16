/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite Cc lazy-writer race user-mode part
 * PROGRAMMER:      ReactOS Developers
 */

#include <kmt_test.h>

START_TEST(CcLazyWriteRace)
{
    HANDLE Handle;
    NTSTATUS Status;
    LARGE_INTEGER ByteOffset;
    IO_STATUS_BLOCK IoStatusBlock;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING RaceFile = RTL_CONSTANT_STRING(L"\\Device\\Kmtest-CcLazyWriteRace\\RaceTestFile");
    PVOID Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, 512);
    DWORD Error;

    RtlFillMemory(Buffer, 512, 0x42);

    Error = KmtLoadAndOpenDriver(L"CcLazyWriteRace", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
        return;
    }

    InitializeObjectAttributes(&ObjectAttributes, &RaceFile, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Handle, FILE_ALL_ACCESS, &ObjectAttributes, &IoStatusBlock, 0,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Cached write dirties a VACB and arms the in-kernel race orchestration:
     * the next lazy-write pass parks in AcquireForLazyWrite while a worker
     * thread cleans the same VACB via CcFlushCache.  On a buggy kernel the
     * lazy writer then asserts ASSERT(Vacb->Dirty) in cc/view.c and bugchecks.
     * On a fixed kernel the now-clean VACB is simply skipped. */
    ByteOffset.QuadPart = 0;
    Status = NtWriteFile(Handle, NULL, NULL, NULL, &IoStatusBlock, Buffer, 512, &ByteOffset, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Give the periodic lazy writer time to fire and run the race. If the
     * kernel is buggy the machine bugchecks during this wait. */
    Sleep(10000);

    /* Reaching here means the race did not assert: the fix is in place. */
    ok(TRUE, "Lazy-writer race did not bugcheck (kernel is fixed)\n");

    NtClose(Handle);

    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
    KmtCloseDriver();
    KmtUnloadDriver();
}
