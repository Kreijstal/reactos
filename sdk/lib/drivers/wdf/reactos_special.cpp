#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ntverp.h>

extern "C" {
#include <ntddk.h>
#include <ntstrsafe.h>
}

#include <wdf.h>

#if (DLL_EXPORT_VERSION < 0xA00)
/* Preserve ExAllocatePool2 semantics using the legacy kernel allocator. */
extern "C"
PVOID
NTAPI
FxAllocatePool2(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag)
{
    const POOL_FLAGS TypeFlags = POOL_FLAG_NON_PAGED |
                                 POOL_FLAG_NON_PAGED_EXECUTE |
                                 POOL_FLAG_PAGED;
    const POOL_FLAGS KnownRequiredFlags = TypeFlags |
                                          POOL_FLAG_USE_QUOTA |
                                          POOL_FLAG_UNINITIALIZED |
                                          POOL_FLAG_SESSION |
                                          POOL_FLAG_CACHE_ALIGNED |
                                          POOL_FLAG_RAISE_ON_FAILURE;
    const POOL_FLAGS RequiredFlagsMask = (POOL_FLAG_REQUIRED_END << 1) - 1;
    POOL_FLAGS Type = Flags & TypeFlags;
    POOL_TYPE PoolType;
    PVOID Buffer;

    if ((Type == 0) || (Type & (Type - 1)) ||
        (Flags & RequiredFlagsMask & ~KnownRequiredFlags) || (Tag == 0))
    {
        if (Flags & POOL_FLAG_RAISE_ON_FAILURE)
            ExRaiseStatus(STATUS_INVALID_PARAMETER);
        return NULL;
    }

    PoolType = (Type == POOL_FLAG_PAGED) ? PagedPool : NonPagedPool;
    if (Flags & POOL_FLAG_CACHE_ALIGNED)
        PoolType = (POOL_TYPE)(PoolType | NonPagedPoolCacheAligned);
    if (Flags & POOL_FLAG_SESSION)
        PoolType = (POOL_TYPE)(PoolType | NonPagedPoolSession);

    if (Flags & POOL_FLAG_USE_QUOTA)
        Buffer = ExAllocatePoolWithQuotaTag((POOL_TYPE)(PoolType | POOL_QUOTA_FAIL_INSTEAD_OF_RAISE),
                                           NumberOfBytes, Tag);
    else
        Buffer = ExAllocatePoolWithTag(PoolType, NumberOfBytes, Tag);

    if (!Buffer)
    {
        if (Flags & POOL_FLAG_RAISE_ON_FAILURE)
            ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
        return NULL;
    }
    if (!(Flags & POOL_FLAG_UNINITIALIZED))
        RtlZeroMemory(Buffer, NumberOfBytes);
    return Buffer;
}
#endif

#define  FX_DYNAMICS_GENERATE_TABLE   1

//-----------------------------------------    ------------------------------------

extern "C" {

typedef VOID (NTAPI *WDFFUNC) (VOID);
#define  KMDF_DEFAULT_NAME   "Wdf01000"

void
__cxa_pure_virtual()
{
	__debugbreak();
}

}  // extern "C"
