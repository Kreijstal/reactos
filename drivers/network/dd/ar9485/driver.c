/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     NDIS 6.20 DriverEntry and miniport registration.
 *
 * Phase 1a: register the NDIS 6 miniport so PnP can bind to
 * PCI\VEN_168C&DEV_0032.  All datapath/scan/connect handlers are
 * stubs that return NDIS_STATUS_NOT_SUPPORTED for now.
 */

#include "ar9485.h"

#define NDEBUG
#include <debug.h>

NDIS_HANDLE g_NdisMiniportDriverHandle = NULL;

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS MiniportChars;
    NDIS_STATUS Status;

    DPRINT1("AR9485: DriverEntry, NDIS 6.%u\n", NDIS_MINIPORT_MINOR_VERSION);

    NdisZeroMemory(&MiniportChars, sizeof(MiniportChars));

    MiniportChars.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    MiniportChars.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    MiniportChars.Header.Size = sizeof(NDIS_MINIPORT_DRIVER_CHARACTERISTICS);

    MiniportChars.MajorNdisVersion   = NDIS_MINIPORT_MAJOR_VERSION;
    MiniportChars.MinorNdisVersion   = NDIS_MINIPORT_MINOR_VERSION;
    MiniportChars.MajorDriverVersion = 0;
    MiniportChars.MinorDriverVersion = 1;

    MiniportChars.InitializeHandlerEx        = AR9485MiniportInitializeEx;
    MiniportChars.HaltHandlerEx              = AR9485MiniportHaltEx;
    MiniportChars.UnloadHandler              = AR9485MiniportDriverUnload;
    MiniportChars.PauseHandler               = AR9485MiniportPause;
    MiniportChars.RestartHandler             = AR9485MiniportRestart;
    MiniportChars.OidRequestHandler          = AR9485OidRequest;
    MiniportChars.SendNetBufferListsHandler  = AR9485SendNetBufferLists;
    MiniportChars.ReturnNetBufferListsHandler= AR9485ReturnNetBufferLists;
    MiniportChars.CancelSendHandler          = AR9485CancelSend;
    MiniportChars.DevicePnPEventNotifyHandler= AR9485MiniportDevicePnPEventNotify;
    MiniportChars.ShutdownHandlerEx          = AR9485MiniportShutdownEx;
    MiniportChars.CancelOidRequestHandler    = AR9485CancelOidRequest;

    Status = NdisMRegisterMiniportDriver(DriverObject,
                                         RegistryPath,
                                         NULL,
                                         &MiniportChars,
                                         &g_NdisMiniportDriverHandle);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("AR9485: NdisMRegisterMiniportDriver failed 0x%08x\n", Status);
        return Status;
    }

    DPRINT1("AR9485: registered NDIS 6.%u miniport, handle=%p\n",
            NDIS_MINIPORT_MINOR_VERSION, g_NdisMiniportDriverHandle);
    return STATUS_SUCCESS;
}

VOID NTAPI
AR9485MiniportDriverUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    DPRINT1("AR9485: DriverUnload\n");
    if (g_NdisMiniportDriverHandle != NULL)
    {
        NdisMDeregisterMiniportDriver(g_NdisMiniportDriverHandle);
        g_NdisMiniportDriverHandle = NULL;
    }
}
