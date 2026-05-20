/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Phase 1a interrupt scaffolding.  Registers an ISR that
 *              acknowledges nothing and a DPC that does nothing; full
 *              ISR/DPC arrive once the RX/TX rings land in Phase 4.
 */

#include "ar9485.h"

#define NDEBUG
#include <debug.h>

NDIS_STATUS
AR9485RegisterInterrupt(_In_ PAR9485_ADAPTER Adapter)
{
    NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS IntChars;

    NdisZeroMemory(&IntChars, sizeof(IntChars));
    IntChars.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_INTERRUPT;
    IntChars.Header.Revision = NDIS_MINIPORT_INTERRUPT_REVISION_1;
    IntChars.Header.Size     = sizeof(IntChars);

    IntChars.InterruptHandler        = AR9485Isr;
    IntChars.InterruptDpcHandler     = AR9485InterruptDpc;
    IntChars.DisableInterruptHandler = AR9485DisableInterrupt;
    IntChars.EnableInterruptHandler  = AR9485EnableInterrupt;
    IntChars.MsiSupported            = Adapter->HasMessageInterrupt;
    IntChars.MsiSyncWithAllMessages  = Adapter->HasMessageInterrupt;

    return NdisMRegisterInterruptEx(Adapter->MiniportAdapterHandle,
                                    Adapter,
                                    &IntChars,
                                    &Adapter->InterruptHandle);
}

VOID
AR9485UnregisterInterrupt(_In_ PAR9485_ADAPTER Adapter)
{
    if (Adapter->InterruptHandle != NULL)
    {
        NdisMDeregisterInterruptEx(Adapter->InterruptHandle);
        Adapter->InterruptHandle = NULL;
    }
}

BOOLEAN NTAPI
AR9485Isr(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _Out_ PBOOLEAN QueueDefaultInterruptDpc,
    _Out_ PULONG TargetProcessors)
{
    UNREFERENCED_PARAMETER(MiniportInterruptContext);

    /* Phase 1a: interrupts are masked at the hardware level (we never
     * enable any IMR bit), so a spurious assertion is the only way we
     * land here.  Tell NDIS this isn't ours so other devices sharing
     * the line still get a look. */
    *QueueDefaultInterruptDpc = FALSE;
    *TargetProcessors = 0;
    return FALSE;
}

VOID NTAPI
AR9485InterruptDpc(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _In_ PVOID MiniportDpcContext,
    _In_ PVOID ReceiveThrottleParameters,
    _In_ PVOID NdisReserved2)
{
    UNREFERENCED_PARAMETER(MiniportInterruptContext);
    UNREFERENCED_PARAMETER(MiniportDpcContext);
    UNREFERENCED_PARAMETER(ReceiveThrottleParameters);
    UNREFERENCED_PARAMETER(NdisReserved2);
}

VOID NTAPI
AR9485DisableInterrupt(_In_ NDIS_HANDLE MiniportInterruptContext)
{
    UNREFERENCED_PARAMETER(MiniportInterruptContext);
}

VOID NTAPI
AR9485EnableInterrupt(_In_ NDIS_HANDLE MiniportInterruptContext)
{
    UNREFERENCED_PARAMETER(MiniportInterruptContext);
}
