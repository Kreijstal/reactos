/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Packet and fragment ring buffers
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#include <net/packet.h>
#include <net/fragment.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DECLSPEC_CACHEALIGN _NET_RING
{
    UINT16 OSReserved1;
    /* Byte distance between two consecutive elements */
    UINT16 ElementStride;
    /* Always a power of two */
    UINT32 NumberOfElements;
    UINT32 ElementIndexMask;
    UINT32 EndIndex;
    union
    {
        UINT32 OSReserved0;
        void * OSReserved2[4];
    } DUMMYUNIONNAME;
    UINT32 BeginIndex;
    UINT32 NextIndex;
    void * Scratch;
    unsigned char Buffer[ANYSIZE_ARRAY];
} NET_RING;

/* Wrap-around arithmetic: NumberOfElements is a power of two */
FORCEINLINE
UINT32
NetRingIncrementIndex(
    _In_ NET_RING const * Ring,
    _In_ UINT32 Index)
{
    return (Index + 1) & Ring->ElementIndexMask;
}

FORCEINLINE
UINT32
NetRingAdvanceIndex(
    _In_ NET_RING const * Ring,
    _In_ UINT32 Index,
    _In_ INT32 Distance)
{
    return (Index + (UINT32)Distance) & Ring->ElementIndexMask;
}

FORCEINLINE
UINT32
NetRingGetRangeCount(
    _In_ NET_RING const * Ring,
    _In_ UINT32 StartIndex,
    _In_ UINT32 EndIndex)
{
    return (EndIndex - StartIndex) & Ring->ElementIndexMask;
}

FORCEINLINE
void *
NetRingGetElementAtIndex(
    _In_ NET_RING const * Ring,
    _In_ UINT32 Index)
{
    return (void *)&Ring->Buffer[(SIZE_T)Index * Ring->ElementStride];
}

FORCEINLINE
NET_PACKET *
NetRingGetPacketAtIndex(
    _In_ NET_RING const * Ring,
    _In_ UINT32 Index)
{
    return (NET_PACKET *)NetRingGetElementAtIndex(Ring, Index);
}

FORCEINLINE
NET_FRAGMENT *
NetRingGetFragmentAtIndex(
    _In_ NET_RING const * Ring,
    _In_ UINT32 Index)
{
    return (NET_FRAGMENT *)NetRingGetElementAtIndex(Ring, Index);
}

#ifdef __cplusplus
}
#endif
