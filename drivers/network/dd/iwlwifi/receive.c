/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Receive path placeholder.
 */

#include "iwlwifi.h"

#define NDEBUG
#include <debug.h>

VOID NTAPI
IwlReturnNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReturnFlags)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(ReturnFlags);

    /* This driver never indicates a receive, so NDIS can never return one.
     * Record the invariant rather than defending against it. */
    UNREFERENCED_PARAMETER(NetBufferLists);
    NT_ASSERT(NetBufferLists == NULL);
}
