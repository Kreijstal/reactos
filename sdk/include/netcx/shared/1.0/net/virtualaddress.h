/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Fragment virtual address extension accessor
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#include <net/extension.h>
#include <net/virtualaddresstypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME L"ms_fragment_virtualaddress"
#define NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1 1U

FORCEINLINE
NET_FRAGMENT_VIRTUAL_ADDRESS *
NetExtensionGetFragmentVirtualAddress(
    _In_ NET_EXTENSION const * Extension,
    _In_ UINT32 Index)
{
    return (NET_FRAGMENT_VIRTUAL_ADDRESS *)NetExtensionGetData(Extension, Index);
}

#ifdef __cplusplus
}
#endif
