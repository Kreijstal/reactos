/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private NDIS interface for the Network Adapter Class Extension
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * NetAdapterCx registers each KMDF client driver as an NDIS 6 miniport
 * driver and each NETADAPTER as an adapter of it (see ndiswdf.h). The
 * miniport handlers belong to the class extension; this file only maps the
 * class extension's PnP notifications onto the adapter lifecycle the rest
 * of the NDIS 6 bridge already implements (60adapter.c):
 *
 *   NdisWdfPnPAddDevice      -> LOGICAL_ADAPTER in a device object of ours
 *   NdisWdfActionPnpStart    -> MiniportInitializeEx, Restart, protocol binds
 *   NdisWdfActionPnpStop     -> Pause, MiniportHaltEx
 *   NdisWdfActionPnpRemove   -> halt if needed, free the adapter
 */

#include "ndis6_internal.h"
#include <drivers/ndis/ndiswdf.h>

#define NDIS_WDF_TAG 'xCWN'

typedef struct _NDIS_WDF_CX
{
    ULONG Signature;
    PDRIVER_OBJECT CxDriverObject;
} NDIS_WDF_CX, *PNDIS_WDF_CX;

#define NDIS_WDF_CX_SIGNATURE 'xCdN'

/* NDIS's own driver object: owner of the NetAdapterCx adapter devices */
static PDRIVER_OBJECT NdisWdfDriverObject;

extern VOID Ndis6BindAllProtocolsToAdapter(PLOGICAL_ADAPTER Adapter);
extern VOID Ndis6AttachFiltersToAdapter(PLOGICAL_ADAPTER Adapter);
extern VOID ndisRebindAllProtocols(VOID);

static
NTSTATUS
NTAPI
NdisWdfDispatchAdapterDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    NTSTATUS Status;

    /* Create/close and IOCTL_NDIS_QUERY_GLOBAL_STATS, as on a miniport FDO */
    if (Ndis6TryDispatchAdapterFdoIrp(DeviceObject, Irp, &Status))
        return Status;

    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_REQUEST;
}

VOID
Ndis6WdfInit(
    _In_ PDRIVER_OBJECT NdisDriverObject)
{
    PDEVICE_OBJECT KeepAlive;
    NTSTATUS Status;

    /*
     * The I/O manager unloads a driver when its last device object goes
     * away. NDIS is a library as much as a driver and must stay, so it
     * keeps a device of its own besides the NetAdapterCx adapter devices.
     */
    Status = IoCreateDevice(NdisDriverObject, 0, NULL, FILE_DEVICE_NETWORK,
                            FILE_DEVICE_SECURE_OPEN, FALSE, &KeepAlive);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("NDIS6: no NDIS device (0x%08lx), NetAdapterCx is not available\n", Status);
        return;
    }
    KeepAlive->Flags &= ~DO_DEVICE_INITIALIZING;

    NdisWdfDriverObject = NdisDriverObject;

    /* NDIS creates no other devices of its own */
    NdisDriverObject->MajorFunction[IRP_MJ_CREATE] = NdisWdfDispatchAdapterDevice;
    NdisDriverObject->MajorFunction[IRP_MJ_CLEANUP] = NdisWdfDispatchAdapterDevice;
    NdisDriverObject->MajorFunction[IRP_MJ_CLOSE] = NdisWdfDispatchAdapterDevice;
    NdisDriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = NdisWdfDispatchAdapterDevice;
}

static
PLOGICAL_ADAPTER
NdisWdfGetAdapter(
    _In_ NDIS_HANDLE NdisMiniportHandle)
{
    PLOGICAL_ADAPTER Adapter = (PLOGICAL_ADAPTER)NdisMiniportHandle;
    PNDIS6_ADAPTER_EXT Ext;

    if (Adapter == NULL || !Adapter->IsNdis6)
        return NULL;

    Ext = NDIS6_EXT(Adapter);
    if (Ext == NULL || !Ext->IsNetCx)
        return NULL;

    return Adapter;
}

NDIS_STATUS
NTAPI
NdisWdfRegisterCx(
    _In_ PNDIS_WDF_CX_CHARACTERISTICS CxCharacteristics,
    _Out_ PNDIS_HANDLE NdisCxHandle)
{
    PNDIS_WDF_CX Cx;

    if (CxCharacteristics == NULL || NdisCxHandle == NULL ||
        CxCharacteristics->Size < sizeof(NDIS_WDF_CX_CHARACTERISTICS) ||
        CxCharacteristics->Version != NDIS_WDF_CX_CHARACTERISTICS_VERSION_1)
    {
        return NDIS_STATUS_INVALID_PARAMETER;
    }

    *NdisCxHandle = NULL;

    if (NdisWdfDriverObject == NULL)
    {
        DbgPrint("NDIS6: NdisWdfRegisterCx before NDIS initialized\n");
        return NDIS_STATUS_NOT_ACCEPTED;
    }

    Cx = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Cx), NDIS_WDF_TAG);
    if (Cx == NULL)
        return NDIS_STATUS_RESOURCES;

    Cx->Signature = NDIS_WDF_CX_SIGNATURE;
    Cx->CxDriverObject = CxCharacteristics->CxDriverObject;

    DbgPrint("NDIS6: class extension %wZ registered\n",
             Cx->CxDriverObject ? &Cx->CxDriverObject->DriverName : NULL);

    *NdisCxHandle = (NDIS_HANDLE)Cx;
    return NDIS_STATUS_SUCCESS;
}

VOID
NTAPI
NdisWdfDeregisterCx(
    _In_ NDIS_HANDLE NdisCxHandle)
{
    PNDIS_WDF_CX Cx = (PNDIS_WDF_CX)NdisCxHandle;

    if (Cx == NULL || Cx->Signature != NDIS_WDF_CX_SIGNATURE)
        return;

    Cx->Signature = 0;
    ExFreePoolWithTag(Cx, NDIS_WDF_TAG);
}

NDIS_STATUS
NTAPI
NdisWdfRegisterMiniportDriver(
    _In_ NDIS_HANDLE NdisCxHandle,
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PUNICODE_STRING RegistryPath,
    _In_opt_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_DRIVER_CHARACTERISTICS MiniportDriverCharacteristics,
    _Out_ PNDIS_HANDLE NdisMiniportDriverHandle)
{
    PNDIS_WDF_CX Cx = (PNDIS_WDF_CX)NdisCxHandle;
    PNDIS6_DRIVER_BLOCK Block;
    NDIS_STATUS Status;

    if (Cx == NULL || Cx->Signature != NDIS_WDF_CX_SIGNATURE ||
        NdisMiniportDriverHandle == NULL)
    {
        return NDIS_STATUS_INVALID_PARAMETER;
    }

    *NdisMiniportDriverHandle = NULL;

    Status = Ndis6RegisterMiniportDriverBlock(DriverObject,
                                              RegistryPath,
                                              MiniportDriverContext,
                                              MiniportDriverCharacteristics,
                                              FALSE,
                                              &Block);
    if (Status != NDIS_STATUS_SUCCESS)
        return Status;

    DbgPrint("NDIS6: NetAdapterCx client %wZ registered as miniport driver\n",
             &DriverObject->DriverName);

    *NdisMiniportDriverHandle = (NDIS_HANDLE)Block;
    return NDIS_STATUS_SUCCESS;
}

VOID
NTAPI
NdisWdfDeregisterMiniportDriver(
    _In_ NDIS_HANDLE NdisMiniportDriverHandle)
{
    PNDIS6_DRIVER_BLOCK Block = (PNDIS6_DRIVER_BLOCK)NdisMiniportDriverHandle;

    if (Block == NULL || !Block->IsNetCx)
        return;

    NdisMDeregisterMiniportDriver(NdisMiniportDriverHandle);
}

NDIS_STATUS
NTAPI
NdisWdfPnPAddDevice(
    _In_ PNDIS_WDF_ADD_DEVICE_INFO AddDeviceInfo,
    _Out_ PNDIS_HANDLE NdisMiniportHandle)
{
    PNDIS6_DRIVER_BLOCK Block;
    PLOGICAL_ADAPTER Adapter;
    NDIS_STATUS Status;

    if (AddDeviceInfo == NULL || NdisMiniportHandle == NULL ||
        AddDeviceInfo->Size < sizeof(NDIS_WDF_ADD_DEVICE_INFO))
    {
        return NDIS_STATUS_INVALID_PARAMETER;
    }

    *NdisMiniportHandle = NULL;

    Block = (PNDIS6_DRIVER_BLOCK)AddDeviceInfo->MiniportDriverHandle;
    if (Block == NULL || !Block->IsNetCx || AddDeviceInfo->PhysicalDeviceObject == NULL)
        return NDIS_STATUS_INVALID_PARAMETER;

    if (NdisWdfDriverObject == NULL)
        return NDIS_STATUS_NOT_ACCEPTED;

    Status = Ndis6CreateNetCxAdapter(Block,
                                     NdisWdfDriverObject,
                                     AddDeviceInfo->PhysicalDeviceObject,
                                     AddDeviceInfo->FunctionalDeviceObject,
                                     AddDeviceInfo->MiniportAddDeviceContext,
                                     &Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
        return Status;

    *NdisMiniportHandle = (NDIS_HANDLE)Adapter;
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
NdisWdfStartAdapter(
    _In_ PLOGICAL_ADAPTER Adapter)
{
    PNDIS6_ADAPTER_EXT Ext = NDIS6_EXT(Adapter);
    NDIS_STATUS Status;

    if (Ext->Initialized)
        return NDIS_STATUS_SUCCESS;

    Status = Ndis6CallMiniportInitializeEx(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DbgPrint("NDIS6: NetAdapterCx adapter %wZ failed to initialize 0x%08lx\n",
                 &Adapter->NdisMiniportBlock.MiniportName, Status);
        return Status;
    }

    Ext->Initialized = TRUE;

    /* Same steps as a started PnP miniport (Ndis6DispatchPnp) */
    Ndis6ImSeedTcpipRegistry(&Adapter->NdisMiniportBlock.MiniportName);
    Ndis6CallMiniportRestartEx(Adapter);
    Ndis6BindAllProtocolsToAdapter(Adapter);
    Ndis6AttachFiltersToAdapter(Adapter);
    ndisRebindAllProtocols();

    return NDIS_STATUS_SUCCESS;
}

static
VOID
NdisWdfHaltAdapter(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_HALT_ACTION HaltAction)
{
    PNDIS6_ADAPTER_EXT Ext = NDIS6_EXT(Adapter);

    if (!Ext->Initialized)
        return;

    /* Pauses the datapath first */
    Ndis6CallMiniportHaltEx(Adapter, HaltAction);
    Ext->Initialized = FALSE;
}

NDIS_STATUS
NTAPI
NdisWdfPnpPowerEventHandler(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_WDF_PNP_POWER_EVENT Event)
{
    PLOGICAL_ADAPTER Adapter;

    PAGED_CODE();

    Adapter = NdisWdfGetAdapter(NdisMiniportHandle);
    if (Adapter == NULL)
        return NDIS_STATUS_INVALID_PARAMETER;

    switch (Event)
    {
        case NdisWdfActionPnpStart:
            return NdisWdfStartAdapter(Adapter);

        case NdisWdfActionPnpStop:
            NdisWdfHaltAdapter(Adapter, NdisHaltDeviceStopped);
            return NDIS_STATUS_SUCCESS;

        case NdisWdfActionPnpRemove:
        case NdisWdfActionPnpSurpriseRemove:
            NdisWdfHaltAdapter(Adapter,
                               Event == NdisWdfActionPnpSurpriseRemove ?
                                   NdisHaltDeviceSurpriseRemoved :
                                   NdisHaltDeviceRemoved);
            /* Deletes our device object and with it the LOGICAL_ADAPTER */
            Ndis6DestroyLogicalAdapter(Adapter);
            return NDIS_STATUS_SUCCESS;

        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }
}

NDIS_HANDLE
NTAPI
NdisWdfGetAdapterContextFromAdapterHandle(
    _In_ NDIS_HANDLE NdisMiniportHandle)
{
    PLOGICAL_ADAPTER Adapter = NdisWdfGetAdapter(NdisMiniportHandle);

    if (Adapter == NULL)
        return NULL;

    return NDIS6_EXT(Adapter)->MiniportAdapterContext;
}
