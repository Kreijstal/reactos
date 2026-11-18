#pragma once

#include <ndk/arm64/ketypes.h>

#ifdef __cplusplus
extern "C" {
#endif

FORCEINLINE
VOID
KeSetCurrentIrql(
    _In_ KIRQL NewIrql)
{
    /* x18 holds the PCR from the first instruction of KiSystemStartup on */
    KeGetPcr()->CurrentIrql = NewIrql;
}

#ifdef __cplusplus
}
#endif
