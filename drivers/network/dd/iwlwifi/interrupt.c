/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Interrupt scaffolding.
 *
 * Phase 1a never unmasks a single CSR_INT_MASK bit, so the device cannot
 * raise an interrupt of its own.  The ISR therefore claims nothing - on a
 * shared line that is what lets the real owner see the assertion.
 */

#include "iwlwifi.h"

#define NDEBUG
#include <debug.h>

NDIS_STATUS
IwlRegisterInterrupt(_In_ PIWL_ADAPTER Adapter)
{
    NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS IntChars;

    NdisZeroMemory(&IntChars, sizeof(IntChars));
    IntChars.Header.Type     = NDIS_OBJECT_TYPE_MINIPORT_INTERRUPT;
    IntChars.Header.Revision = NDIS_MINIPORT_INTERRUPT_REVISION_1;
    IntChars.Header.Size     = sizeof(IntChars);

    IntChars.InterruptHandler        = IwlIsr;
    IntChars.InterruptDpcHandler     = IwlInterruptDpc;
    IntChars.DisableInterruptHandler = IwlDisableInterruptHandler;
    IntChars.EnableInterruptHandler  = IwlEnableInterruptHandler;
    IntChars.MsiSupported            = Adapter->HasMessageInterrupt;
    IntChars.MsiSyncWithAllMessages  = Adapter->HasMessageInterrupt;

    return NdisMRegisterInterruptEx(Adapter->MiniportAdapterHandle,
                                    Adapter,
                                    &IntChars,
                                    &Adapter->InterruptHandle);
}

VOID
IwlUnregisterInterrupt(_In_ PIWL_ADAPTER Adapter)
{
    if (Adapter->InterruptHandle != NULL)
    {
        NdisMDeregisterInterruptEx(Adapter->InterruptHandle);
        Adapter->InterruptHandle = NULL;
    }
}

BOOLEAN NTAPI
IwlIsr(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _Out_ PBOOLEAN QueueDefaultInterruptDpc,
    _Out_ PULONG TargetProcessors)
{
    UNREFERENCED_PARAMETER(MiniportInterruptContext);

    *QueueDefaultInterruptDpc = FALSE;
    *TargetProcessors = 0;
    return FALSE;
}

VOID NTAPI
IwlInterruptDpc(
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
IwlDisableInterruptHandler(_In_ NDIS_HANDLE MiniportInterruptContext)
{
    PIWL_ADAPTER Adapter = (PIWL_ADAPTER)MiniportInterruptContext;

    if (Adapter != NULL && Adapter->IoBase != NULL)
        IwlWrite32(Adapter, CSR_INT_MASK, 0);
}

VOID NTAPI
IwlEnableInterruptHandler(_In_ NDIS_HANDLE MiniportInterruptContext)
{
    /* Nothing to re-enable: Phase 1a keeps CSR_INT_MASK at zero. */
    UNREFERENCED_PARAMETER(MiniportInterruptContext);
}
