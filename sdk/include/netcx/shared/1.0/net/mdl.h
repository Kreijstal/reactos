/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Fragment MDL extension accessor
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#include <net/extension.h>
#include <net/mdltypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_FRAGMENT_EXTENSION_MDL_NAME L"ms_fragment_mdl"
#define NET_FRAGMENT_EXTENSION_MDL_VERSION_1 1U

FORCEINLINE
NET_FRAGMENT_MDL *
NetExtensionGetFragmentMdl(
    _In_ NET_EXTENSION const * Extension,
    _In_ UINT32 Index)
{
    return (NET_FRAGMENT_MDL *)NetExtensionGetData(Extension, Index);
}

#ifdef __cplusplus
}
#endif
