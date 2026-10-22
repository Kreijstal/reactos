/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Extension queries for packet queues
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifndef _NETADAPTERCXTYPES_H_
#error netadaptercxtypes.h must be included before netadapterpacket.h
#endif

#include <net/extension.h>

WDF_EXTERN_C_START

typedef struct _NET_EXTENSION_QUERY
{
    ULONG Size;
    PCWSTR Name;
    ULONG Version;
    NET_EXTENSION_TYPE Type;
} NET_EXTENSION_QUERY;

FORCEINLINE
VOID
NET_EXTENSION_QUERY_INIT(
    _Out_ NET_EXTENSION_QUERY * Extension,
    _In_ PCWSTR Name,
    _In_ ULONG Version,
    _In_ NET_EXTENSION_TYPE Type)
{
    RtlZeroMemory(Extension, sizeof(*Extension));
    Extension->Size = sizeof(*Extension);
    Extension->Name = Name;
    Extension->Version = Version;
    Extension->Type = Type;
}

WDF_EXTERN_C_END
