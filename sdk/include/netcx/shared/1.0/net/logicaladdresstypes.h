/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Fragment DMA logical address extension types
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_FRAGMENT_LOGICAL_ADDRESS
{
    UINT64 LogicalAddress;
} NET_FRAGMENT_LOGICAL_ADDRESS;

#ifdef __cplusplus
}
#endif
