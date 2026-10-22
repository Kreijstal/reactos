/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Fragment virtual address extension types
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_FRAGMENT_VIRTUAL_ADDRESS
{
    void * VirtualAddress;
} NET_FRAGMENT_VIRTUAL_ADDRESS;

#ifdef __cplusplus
}
#endif
