/*
 * PROJECT:     ReactOS vwifi virtual Wi-Fi miniport
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     RX path stub.  Synthetic beacon indication for scan will be
 *              added in a follow-up that wires up the beacon-injection
 *              timer; for now no upcalls happen.
 */

#include "vwifi.h"

#define NDEBUG
#include <debug.h>

VOID
NTAPI
VwifiReturnNetBufferLists(
    _In_ NDIS_HANDLE Ctx,
    _In_ PNET_BUFFER_LIST NblChain,
    _In_ ULONG ReturnFlags)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(NblChain);
    UNREFERENCED_PARAMETER(ReturnFlags);
    /* No outstanding NBLs yet because we haven't indicated any. */
}
