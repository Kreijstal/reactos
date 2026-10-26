/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite ExAllocatePool2 test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

#if (NTDDI_VERSION >= NTDDI_WIN10)

#define TAG_POOL2 '2lPK'

static
BOOLEAN
IsZeroed(
    _In_reads_bytes_(Size) PVOID Buffer,
    _In_ SIZE_T Size)
{
    return RtlCompareMemoryUlong(Buffer, Size & ~(SIZE_T)3, 0) == (Size & ~(SIZE_T)3);
}

static
VOID
TestZeroing(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T Size)
{
    PUCHAR Buffer;

    /* Dirty some pool of the same size first, so zeroing is observable */
    Buffer = ExAllocatePool2(Flags | POOL_FLAG_UNINITIALIZED, Size, TAG_POOL2);
    ok(Buffer != NULL, "Allocation of %Iu bytes failed\n", Size);
    if (!Buffer)
        return;
    RtlFillMemory(Buffer, Size, 0xA5);
    ExFreePoolWithTag(Buffer, TAG_POOL2);

    Buffer = ExAllocatePool2(Flags, Size, TAG_POOL2);
    ok(Buffer != NULL, "Allocation of %Iu bytes failed\n", Size);
    if (!Buffer)
        return;
    ok(IsZeroed(Buffer, Size), "Buffer of %Iu bytes (flags 0x%I64x) is not zeroed\n", Size, Flags);
    if (Size >= PAGE_SIZE)
        ok(((ULONG_PTR)Buffer & (PAGE_SIZE - 1)) == 0, "Large buffer %p not page aligned\n", Buffer);
    ExFreePoolWithTag(Buffer, TAG_POOL2);
}

static
VOID
TestInvalid(
    _In_ POOL_FLAGS Flags,
    _In_ ULONG Tag)
{
    PVOID Buffer;
    NTSTATUS ExceptionStatus = STATUS_SUCCESS;

    Buffer = ExAllocatePool2(Flags, 64, Tag);
    ok(Buffer == NULL, "Flags 0x%I64x tag 0x%lx: got %p, expected NULL\n", Flags, Tag, Buffer);
    if (Buffer)
        ExFreePool(Buffer);

    /* With POOL_FLAG_RAISE_ON_FAILURE the failure is an exception */
    Buffer = NULL;
    _SEH2_TRY
    {
        Buffer = ExAllocatePool2(Flags | POOL_FLAG_RAISE_ON_FAILURE, 64, Tag);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        ExceptionStatus = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    ok(Buffer == NULL, "Flags 0x%I64x tag 0x%lx: raise variant got %p\n", Flags, Tag, Buffer);
    ok(!NT_SUCCESS(ExceptionStatus), "Flags 0x%I64x tag 0x%lx: no exception\n", Flags, Tag);
    if (Buffer)
        ExFreePool(Buffer);
}

START_TEST(ExAllocatePool2)
{
    PVOID Buffer;
    KIRQL OldIrql;

    TestZeroing(POOL_FLAG_NON_PAGED, 24);
    TestZeroing(POOL_FLAG_NON_PAGED, 1000);
    TestZeroing(POOL_FLAG_NON_PAGED, 3 * PAGE_SIZE);
    TestZeroing(POOL_FLAG_PAGED, 200);
    TestZeroing(POOL_FLAG_PAGED, 2 * PAGE_SIZE);
    TestZeroing(POOL_FLAG_NON_PAGED_EXECUTE, 128);
    TestZeroing(POOL_FLAG_NON_PAGED | POOL_FLAG_CACHE_ALIGNED, 100);
    TestZeroing(POOL_FLAG_PAGED | POOL_FLAG_USE_QUOTA, 300);

    /* Cache aligned. The ReactOS pool allocator ignores the cache aligned
     * pool types (ExAllocatePoolWithTag too), so only report the alignment. */
    Buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED | POOL_FLAG_CACHE_ALIGNED, 40, TAG_POOL2);
    ok(Buffer != NULL, "Cache aligned allocation failed\n");
    if (Buffer)
    {
        if ((ULONG_PTR)Buffer & (SYSTEM_CACHE_ALIGNMENT_SIZE - 1))
            trace("%p is not cache aligned\n", Buffer);
        ExFreePoolWithTag(Buffer, TAG_POOL2);
    }

    /* Non-paged allocations work at DISPATCH_LEVEL */
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, 512, TAG_POOL2);
    ok(Buffer != NULL, "DISPATCH_LEVEL allocation failed\n");
    if (Buffer)
    {
        ok(IsZeroed(Buffer, 512), "DISPATCH_LEVEL buffer not zeroed\n");
        ExFreePoolWithTag(Buffer, TAG_POOL2);
    }
    KeLowerIrql(OldIrql);

    /* Unknown optional flags are ignored */
    Buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED | 0x4000000000000000ULL, 32, TAG_POOL2);
    ok(Buffer != NULL, "Unknown optional flag was not ignored\n");
    if (Buffer)
        ExFreePoolWithTag(Buffer, TAG_POOL2);

    /* Invalid requests fail */
    TestInvalid(0, TAG_POOL2);                                          /* no pool type */
    TestInvalid(POOL_FLAG_NON_PAGED | POOL_FLAG_PAGED, TAG_POOL2);      /* two pool types */
    TestInvalid(POOL_FLAG_NON_PAGED | 0x40000000ULL, TAG_POOL2);        /* unknown required flag */
    TestInvalid(POOL_FLAG_NON_PAGED, 0);                                /* zero tag */
}

#else

START_TEST(ExAllocatePool2)
{
    skip(FALSE, "ExAllocatePool2 requires NT 10 or later\n");
}

#endif
