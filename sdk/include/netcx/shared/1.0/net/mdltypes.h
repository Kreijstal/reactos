/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Fragment MDL extension types
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_FRAGMENT_MDL
{
    MDL * Mdl;
} NET_FRAGMENT_MDL;

#ifdef __cplusplus
}
#endif
