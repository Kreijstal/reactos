/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Interrupt scaffolding.
 *
 * Gen3 firmware self-load reports ROM/firmware progress through the legacy
 * CSR causes when MSI-X is unavailable.  Claim only causes asserted by this
 * device, acknowledge them immediately, and leave parsing to the DPC.
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
    PIWL_ADAPTER Adapter = (PIWL_ADAPTER)MiniportInterruptContext;
    ULONG Cause;

    *QueueDefaultInterruptDpc = FALSE;
    *TargetProcessors = 0;
    if (Adapter == NULL || Adapter->IoBase == NULL)
        return FALSE;

    Cause = IwlRead32(Adapter, CSR_INT) &
            (CSR_INT_BIT_ALIVE | CSR_INT_BIT_FH_RX |
             CSR_INT_BIT_SW_RX | CSR_INT_BIT_HW_ERR |
             CSR_INT_BIT_SW_ERR);
    if (Cause == 0 || Cause == 0xffffffff)
        return FALSE;

    IwlWrite32(Adapter, CSR_INT, Cause);
    Adapter->LastInterruptCause |= Cause;
    if (Cause & CSR_INT_BIT_ALIVE)
        InterlockedExchange(&Adapter->FirmwareAlive, 1);
    *QueueDefaultInterruptDpc = TRUE;
    return TRUE;
}

VOID NTAPI
IwlInterruptDpc(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _In_ PVOID MiniportDpcContext,
    _In_ PVOID ReceiveThrottleParameters,
    _In_ PVOID NdisReserved2)
{
    PIWL_ADAPTER Adapter = (PIWL_ADAPTER)MiniportInterruptContext;
    UNREFERENCED_PARAMETER(MiniportDpcContext);
    UNREFERENCED_PARAMETER(ReceiveThrottleParameters);
    UNREFERENCED_PARAMETER(NdisReserved2);
    if (Adapter != NULL)
    {
        DPRINT1("iwlwifi: interrupt cause(s) 0x%08x%s\n",
                Adapter->LastInterruptCause,
                Adapter->FirmwareAlive ? " (ALIVE)" : "");
        if (InterlockedCompareExchange(&Adapter->DataPathReady, 0, 0))
            IwlProcessReceiveCompletions(Adapter, TRUE);
    }
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
    PIWL_ADAPTER Adapter = (PIWL_ADAPTER)MiniportInterruptContext;
    if (Adapter != NULL && Adapter->IoBase != NULL)
        IwlWrite32(Adapter, CSR_INT_MASK,
                   CSR_INT_BIT_ALIVE | CSR_INT_BIT_FH_RX |
                   CSR_INT_BIT_SW_RX | CSR_INT_BIT_HW_ERR |
                   CSR_INT_BIT_SW_ERR);
}
