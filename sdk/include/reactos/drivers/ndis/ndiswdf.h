/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private NDIS interface for the Network Adapter Class Extension
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * NetAdapterCx (netadaptercx.sys) presents each NETADAPTER of a KMDF client
 * driver to NDIS as an ordinary NDIS 6 miniport adapter. The client's
 * WDFDEVICE stays the function device object of the PnP stack: NDIS does
 * not take over the client's dispatch table or AddDevice routine. Instead
 * the class extension registers the client driver object, tells NDIS when
 * an adapter is added and when its device is started and removed, and NDIS
 * calls back into the miniport handlers the class extension registered.
 *
 * Windows has an interface of the same purpose (ndiswdf.h). It is not
 * published, so this is a ReactOS design: only the names follow Windows.
 * Both sides of it live in the ReactOS tree.
 *
 * Include <ndis.h> with NDIS 6 support before this header.
 */

#pragma once

#ifndef _NDISWDF_H_
#define _NDISWDF_H_

#ifdef __cplusplus
extern "C" {
#endif

#define NDIS_WDF_CX_CHARACTERISTICS_VERSION_1 1

typedef struct _NDIS_WDF_CX_CHARACTERISTICS
{
    ULONG Size;
    ULONG Version;
    /* Driver object of the class extension itself */
    PDRIVER_OBJECT CxDriverObject;
} NDIS_WDF_CX_CHARACTERISTICS, *PNDIS_WDF_CX_CHARACTERISTICS;

typedef struct _NDIS_WDF_ADD_DEVICE_INFO
{
    ULONG Size;
    /* Returned by NdisWdfRegisterMiniportDriver for the client driver */
    NDIS_HANDLE MiniportDriverHandle;
    /* Bottom of the PnP stack the client's WDFDEVICE is attached to */
    PDEVICE_OBJECT PhysicalDeviceObject;
    /* The client's WDFDEVICE */
    PDEVICE_OBJECT FunctionalDeviceObject;
    /* Handed back in NDIS_MINIPORT_INIT_PARAMETERS::MiniportAddDeviceContext */
    NDIS_HANDLE MiniportAddDeviceContext;
} NDIS_WDF_ADD_DEVICE_INFO, *PNDIS_WDF_ADD_DEVICE_INFO;

typedef enum _NDIS_WDF_PNP_POWER_EVENT
{
    /* The device is started: initialize the miniport adapter and restart it */
    NdisWdfActionPnpStart = 1,
    /* The adapter is stopped but the device stays: halt the miniport adapter */
    NdisWdfActionPnpStop,
    /* The device is going away: halt and free the NDIS adapter */
    NdisWdfActionPnpRemove,
    NdisWdfActionPnpSurpriseRemove,
} NDIS_WDF_PNP_POWER_EVENT;

/* Registers the class extension. One registration per class extension. */
NDIS_STATUS
NTAPI
NdisWdfRegisterCx(
    _In_ PNDIS_WDF_CX_CHARACTERISTICS CxCharacteristics,
    _Out_ PNDIS_HANDLE NdisCxHandle);

VOID
NTAPI
NdisWdfDeregisterCx(
    _In_ NDIS_HANDLE NdisCxHandle);

/*
 * Registers a client driver as an NDIS 6 miniport driver whose handlers are
 * implemented by the class extension. Unlike NdisMRegisterMiniportDriver it
 * leaves the driver object's AddDevice routine and dispatch table alone.
 */
NDIS_STATUS
NTAPI
NdisWdfRegisterMiniportDriver(
    _In_ NDIS_HANDLE NdisCxHandle,
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PUNICODE_STRING RegistryPath,
    _In_opt_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_DRIVER_CHARACTERISTICS MiniportDriverCharacteristics,
    _Out_ PNDIS_HANDLE NdisMiniportDriverHandle);

VOID
NTAPI
NdisWdfDeregisterMiniportDriver(
    _In_ NDIS_HANDLE NdisMiniportDriverHandle);

/*
 * Creates the NDIS adapter for one NETADAPTER. The adapter is visible in
 * the NDIS adapter list but not initialized until NdisWdfActionPnpStart.
 * NdisMiniportHandle is the handle the miniport passes to NdisM* calls.
 */
NDIS_STATUS
NTAPI
NdisWdfPnPAddDevice(
    _In_ PNDIS_WDF_ADD_DEVICE_INFO AddDeviceInfo,
    _Out_ PNDIS_HANDLE NdisMiniportHandle);

NDIS_STATUS
NTAPI
NdisWdfPnpPowerEventHandler(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_WDF_PNP_POWER_EVENT Event);

/* The MiniportAdapterContext the miniport registered, or NULL */
NDIS_HANDLE
NTAPI
NdisWdfGetAdapterContextFromAdapterHandle(
    _In_ NDIS_HANDLE NdisMiniportHandle);

#ifdef __cplusplus
}
#endif

#endif /* _NDISWDF_H_ */
