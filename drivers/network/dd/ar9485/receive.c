/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Phase 1a RX stub.  No frames are ever indicated, so the
 *              only valid path is NdisMReturnNetBufferLists - and the
 *              upper protocol layer would only call that for NBLs we
 *              indicated in the first place.  Treat any unexpected call
 *              as a NOP; the assertion guards the invariant in debug
 *              builds.
 */

#include "ar9485.h"

#define NDEBUG
#include <debug.h>

VOID NTAPI
AR9485ReturnNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReturnFlags)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(NetBufferLists);
    UNREFERENCED_PARAMETER(ReturnFlags);

    /* We have not indicated any NBLs yet, so this should never fire. */
    NT_ASSERT(NetBufferLists == NULL);
}
