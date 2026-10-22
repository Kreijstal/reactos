/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Packet and fragment extensions
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _NET_EXTENSION_TYPE
{
    NetExtensionTypePacket = 1,
    NetExtensionTypeFragment = 2,
    NetExtensionTypeBuffer,
} NET_EXTENSION_TYPE;

/*
 * Filled in by NetTx/RxQueueGetExtension. The framework stores the address
 * of the extension in element 0 and the ring's element stride in slot 1.
 */
typedef struct _NET_EXTENSION
{
    void * Reserved[4];
    union
    {
        BOOLEAN Enabled;
        void * Reserved1;
    } DUMMYUNIONNAME;
} NET_EXTENSION;

FORCEINLINE
void *
NetExtensionGetData(
    _In_ NET_EXTENSION const * Extension,
    _In_ UINT32 Index)
{
    return (unsigned char *)Extension->Reserved[0] + (SIZE_T)Extension->Reserved[1] * Index;
}

#ifdef __cplusplus
}
#endif
