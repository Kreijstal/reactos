/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Set of rings a packet queue hands to the client
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#include <net/ring.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _NET_RING_TYPE
{
    NetRingTypePacket = 0,
    NetRingTypeFragment = 1,
    NetRingTypeDataBuffer,
} NET_RING_TYPE;

typedef struct _NET_RING_COLLECTION
{
    NET_RING * Rings[NetRingTypeDataBuffer + 1];
} NET_RING_COLLECTION;

FORCEINLINE
NET_RING *
NetRingCollectionGetPacketRing(
    _In_ NET_RING_COLLECTION const * Rings)
{
    return Rings->Rings[NetRingTypePacket];
}

FORCEINLINE
NET_RING *
NetRingCollectionGetFragmentRing(
    _In_ NET_RING_COLLECTION const * Rings)
{
    return Rings->Rings[NetRingTypeFragment];
}

#ifdef __cplusplus
}
#endif
