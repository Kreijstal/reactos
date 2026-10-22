/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Fragment descriptor
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_FRAGMENT
{
    UINT64 ValidLength : 26;
    UINT64 Capacity : 26;
    UINT64 Offset : 10;
    UINT64 Scratch : 1;
    UINT64 OsReserved_Bounced : 1;
} NET_FRAGMENT;

#ifdef __cplusplus
}
#endif
