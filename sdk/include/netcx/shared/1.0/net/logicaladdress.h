/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Fragment DMA logical address extension accessor
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#include <net/extension.h>
#include <net/logicaladdresstypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_NAME L"ms_fragment_logicaladdress"
#define NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_VERSION_1 1U

FORCEINLINE
NET_FRAGMENT_LOGICAL_ADDRESS *
NetExtensionGetFragmentLogicalAddress(
    _In_ NET_EXTENSION const * Extension,
    _In_ UINT32 Index)
{
    return (NET_FRAGMENT_LOGICAL_ADDRESS *)NetExtensionGetData(Extension, Index);
}

#ifdef __cplusplus
}
#endif
