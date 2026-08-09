/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Reproducer for a registry value-resize bugcheck.
 *
 * Overwriting an existing registry value with progressively larger data
 * forces HvReallocateCell() to allocate a bigger data cell, which can grow
 * the hive into a new bin (HvpAddBin).  A latent cmlib bug then trips
 * HvpGetCellHeader()'s ASSERT(CellBlock < Storage[CellType].Length)
 * (hivecell.c) - in a release build that is an out-of-bounds BlockList read
 * and a 0x50 (PAGE_FAULT_IN_NONPAGED_AREA).  This test drives exactly that
 * pattern (grow one value across many sizes, flushing each time) so the
 * fault can be reproduced and debugged deterministically.
 */

#include <kmt_test.h>

#define CMRESIZE_KEY    L"\\Registry\\Machine\\SOFTWARE\\CmResizeTest"
#define CMRESIZE_VALUE  L"Blob"
#define CMRESIZE_MAX    (64 * 1024)   /* grow up to 64 KB */
#define CMRESIZE_STEP   (1 * 1024)    /* in 1 KB increments */
#define CMRESIZE_ROUNDS 3             /* repeat the whole grow sweep */

START_TEST(CmResize)
{
    NTSTATUS Status;
    UNICODE_STRING KeyName;
    UNICODE_STRING ValueName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE KeyHandle = NULL;
    PUCHAR Buffer = NULL;
    ULONG Disposition;
    ULONG Round;
    ULONG Size;
    ULONG Created = 0;

    Buffer = ExAllocatePoolWithTag(PagedPool, CMRESIZE_MAX, 'RmtK');
    if (skip(Buffer != NULL, "Failed to allocate %u-byte value buffer\n",
             (ULONG)CMRESIZE_MAX))
    {
        return;
    }
    RtlFillMemory(Buffer, CMRESIZE_MAX, 0xCD);

    RtlInitUnicodeString(&KeyName, CMRESIZE_KEY);
    InitializeObjectAttributes(&ObjectAttributes, &KeyName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);

    Status = ZwCreateKey(&KeyHandle, KEY_ALL_ACCESS, &ObjectAttributes, 0,
                         NULL, REG_OPTION_NON_VOLATILE, &Disposition);
    if (skip(NT_SUCCESS(Status), "ZwCreateKey failed: 0x%lx\n", Status))
    {
        ExFreePoolWithTag(Buffer, 'RmtK');
        return;
    }

    RtlInitUnicodeString(&ValueName, CMRESIZE_VALUE);

    trace("CMRESIZE: growing %wZ\\%wZ 0..%u bytes, %u rounds\n",
          &KeyName, &ValueName, (ULONG)CMRESIZE_MAX, (ULONG)CMRESIZE_ROUNDS);

    for (Round = 0; Round < CMRESIZE_ROUNDS; ++Round)
    {
        /* Each round walks the value back up from small to large.  The first
           SetValue of a round is therefore an overwrite-with-larger of the
           value left over from the previous round's start, mirroring the
           CheckForLiveCD overwrite pattern. */
        for (Size = CMRESIZE_STEP; Size <= CMRESIZE_MAX; Size += CMRESIZE_STEP)
        {
            Status = ZwSetValueKey(KeyHandle, &ValueName, 0, REG_BINARY,
                                   Buffer, Size);
            if (!NT_SUCCESS(Status))
            {
                ok(FALSE, "ZwSetValueKey(%u) failed: 0x%lx\n", Size, Status);
                goto done;
            }
            ++Created;

            /* Flush forces HvSyncHive to walk the dirty cells - the point at
               which the bad cell index historically asserted. */
            Status = ZwFlushKey(KeyHandle);
            if (!NT_SUCCESS(Status))
            {
                ok(FALSE, "ZwFlushKey(%u) failed: 0x%lx\n", Size, Status);
                goto done;
            }
        }

        /* Shrink back to a single byte so the next round overwrites-larger. */
        Status = ZwSetValueKey(KeyHandle, &ValueName, 0, REG_BINARY, Buffer, 1);
        ok(NT_SUCCESS(Status), "ZwSetValueKey(1) failed: 0x%lx\n", Status);
    }

    trace("CMRESIZE: completed %u set/flush cycles with no bugcheck\n", Created);
    DbgPrint("CMRESIZE-OK: %u set/flush cycles completed\n", Created);
    ok(TRUE, "value-resize sweep completed\n");

done:
    ZwClose(KeyHandle);
    /* Best-effort cleanup of the test key's value. */
    ExFreePoolWithTag(Buffer, 'RmtK');
}
