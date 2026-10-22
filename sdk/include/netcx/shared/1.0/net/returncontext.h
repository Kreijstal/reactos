/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Fragment receive buffer return context extension accessor
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#include <net/extension.h>
#include <net/returncontexttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_FRAGMENT_EXTENSION_RETURN_CONTEXT_NAME L"ms_fragment_returncontext"
#define NET_FRAGMENT_EXTENSION_RETURN_CONTEXT_VERSION_1 1U

FORCEINLINE
NET_FRAGMENT_RETURN_CONTEXT *
NetExtensionGetFragmentReturnContext(
    _In_ NET_EXTENSION const * Extension,
    _In_ UINT32 Index)
{
    return (NET_FRAGMENT_RETURN_CONTEXT *)NetExtensionGetData(Extension, Index);
}

#ifdef __cplusplus
}
#endif
