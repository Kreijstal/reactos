/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Fragment receive buffer return context extension types
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

DECLARE_HANDLE(NET_FRAGMENT_RETURN_CONTEXT_HANDLE);

typedef struct _NET_FRAGMENT_RETURN_CONTEXT
{
    NET_FRAGMENT_RETURN_CONTEXT_HANDLE Handle;
} NET_FRAGMENT_RETURN_CONTEXT;

#ifdef __cplusplus
}
#endif
