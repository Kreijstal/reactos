/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     NDIS 6.20 DriverEntry and miniport registration.
 */

#include "iwlwifi.h"

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

    DPRINT1("iwlwifi: DriverEntry, NDIS 6.%u\n", NDIS_MINIPORT_MINOR_VERSION);

    NdisZeroMemory(&MiniportChars, sizeof(MiniportChars));

    MiniportChars.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    MiniportChars.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    MiniportChars.Header.Size = sizeof(NDIS_MINIPORT_DRIVER_CHARACTERISTICS);

    MiniportChars.MajorNdisVersion   = NDIS_MINIPORT_MAJOR_VERSION;
    MiniportChars.MinorNdisVersion   = NDIS_MINIPORT_MINOR_VERSION;
    MiniportChars.MajorDriverVersion = 0;
    MiniportChars.MinorDriverVersion = 1;

    MiniportChars.InitializeHandlerEx         = IwlMiniportInitializeEx;
    MiniportChars.HaltHandlerEx               = IwlMiniportHaltEx;
    MiniportChars.UnloadHandler               = IwlMiniportDriverUnload;
    MiniportChars.PauseHandler                = IwlMiniportPause;
    MiniportChars.RestartHandler              = IwlMiniportRestart;
    MiniportChars.OidRequestHandler           = IwlOidRequest;
    MiniportChars.SendNetBufferListsHandler   = IwlSendNetBufferLists;
    MiniportChars.ReturnNetBufferListsHandler = IwlReturnNetBufferLists;
    MiniportChars.CancelSendHandler           = IwlCancelSend;
    MiniportChars.DevicePnPEventNotifyHandler = IwlMiniportDevicePnPEventNotify;
    MiniportChars.ShutdownHandlerEx           = IwlMiniportShutdownEx;
    MiniportChars.CancelOidRequestHandler     = IwlCancelOidRequest;

    Status = NdisMRegisterMiniportDriver(DriverObject,
                                         RegistryPath,
                                         NULL,
                                         &MiniportChars,
                                         &g_NdisMiniportDriverHandle);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("iwlwifi: NdisMRegisterMiniportDriver failed 0x%08x\n", Status);
        return Status;
    }

    DPRINT1("iwlwifi: registered NDIS 6.%u miniport, handle=%p\n",
            NDIS_MINIPORT_MINOR_VERSION, g_NdisMiniportDriverHandle);
    return STATUS_SUCCESS;
}

VOID NTAPI
IwlMiniportDriverUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    DPRINT1("iwlwifi: DriverUnload\n");
    if (g_NdisMiniportDriverHandle != NULL)
    {
        NdisMDeregisterMiniportDriver(g_NdisMiniportDriverHandle);
        g_NdisMiniportDriverHandle = NULL;
    }
}
