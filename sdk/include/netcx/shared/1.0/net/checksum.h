/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Packet checksum offload extension accessor
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#include <net/extension.h>
#include <net/checksumtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_PACKET_EXTENSION_CHECKSUM_NAME L"ms_packet_checksum"
#define NET_PACKET_EXTENSION_CHECKSUM_VERSION_1 1U

FORCEINLINE
NET_PACKET_CHECKSUM *
NetExtensionGetPacketChecksum(
    _In_ NET_EXTENSION const * Extension,
    _In_ UINT32 Index)
{
    return (NET_PACKET_CHECKSUM *)NetExtensionGetData(Extension, Index);
}

#ifdef __cplusplus
}
#endif
