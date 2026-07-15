/*
 * PROJECT:     ReactOS HD Audio codec function driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PortCls adapter driver binding HDAUDIO function group PDOs
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/* instantiate all GUIDs referenced by the driver in this translation unit */
#include <initguid.h>

#include "driver.h"

#define NDEBUG
#include <debug.h>

#define MAX_MINIPORTS 2

/* wave filter bridge pin (see wave.cpp) */
#define WAVE_PIN_RENDER_BRIDGE  1
/* topology filter source pin (see topology.cpp) */
#define TOPO_PIN_WAVEOUT_SOURCE 0

extern "C" DRIVER_INITIALIZE DriverEntry;
static DRIVER_ADD_DEVICE AddDevice;

/*****************************************************************************
 * InstallSubdevice
 *****************************************************************************
 * Creates and registers a port/miniport pair (same shape as the ac97
 * sample adapter).
 */
static
NTSTATUS
InstallSubdevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PCWSTR Name,
    _In_ REFGUID PortClassId,
    _In_ REFGUID MiniportClassId,
    _In_opt_ PFNCREATEMINIPORT MiniportCreate,
    _In_opt_ PUNKNOWN UnknownAdapter,
    _In_opt_ PRESOURCELIST ResourceList,
    _Out_opt_ PUNKNOWN *OutPortUnknown)
{
    NTSTATUS Status;
    PPORT Port;
    PMINIPORT Miniport;

    PAGED_CODE();

    Status = PcNewPort(&Port, PortClassId);
    if (!NT_SUCCESS(Status))
        return Status;

    if (MiniportCreate)
    {
        Status = MiniportCreate((PUNKNOWN *)&Miniport, MiniportClassId,
                                NULL, NonPagedPool);
    }
    else
    {
        Status = PcNewMiniport(&Miniport, MiniportClassId);
    }

    if (!NT_SUCCESS(Status))
    {
        Port->Release();
        return Status;
    }

    Status = Port->Init(DeviceObject, Irp, Miniport, UnknownAdapter, ResourceList);
    if (NT_SUCCESS(Status))
    {
        /* PcRegisterSubdevice takes a non-const PWCHAR but does not modify it */
        Status = PcRegisterSubdevice(DeviceObject, (PWCHAR)Name, Port);

        if (OutPortUnknown && NT_SUCCESS(Status))
        {
            Status = Port->QueryInterface(IID_IUnknown, (PVOID *)OutPortUnknown);
        }
    }

    Miniport->Release();
    Port->Release();

    return Status;
}

/*****************************************************************************
 * StartDevice
 */
static
NTSTATUS
NTAPI
StartDevice(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PRESOURCELIST ResourceList)
{
    PUNKNOWN UnknownCodec = NULL;
    PHDACODEC Codec = NULL;
    PUNKNOWN UnknownWave = NULL;
    PUNKNOWN UnknownTopology = NULL;
    NTSTATUS Status;

    PAGED_CODE();

    DPRINT1("HDAUDIO: StartDevice\n");

    /* create and initialize the codec object */
    Status = NewHDACodec(&UnknownCodec, NULL, NonPagedPool);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UnknownCodec->QueryInterface(IID_IHDACodec, (PVOID *)&Codec);
    if (!NT_SUCCESS(Status))
    {
        UnknownCodec->Release();
        return Status;
    }

    Status = Codec->Init(DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HDAUDIO: codec initialization failed: 0x%lx\n", Status);
        goto cleanup;
    }

    /* install the topology subdevice */
    Status = InstallSubdevice(DeviceObject,
                              Irp,
                              L"Topology",
                              CLSID_PortTopology,
                              CLSID_PortTopology, /* not used */
                              CreateMiniportTopologyHDA,
                              UnknownCodec,
                              NULL,
                              &UnknownTopology);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HDAUDIO: failed to install topology subdevice: 0x%lx\n", Status);
        goto cleanup;
    }

    /* install the wave subdevice */
    Status = InstallSubdevice(DeviceObject,
                              Irp,
                              L"Wave",
                              CLSID_PortWaveCyclic,
                              CLSID_PortWaveCyclic, /* not used */
                              CreateMiniportWaveCyclicHDA,
                              UnknownCodec,
                              ResourceList,
                              &UnknownWave);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HDAUDIO: failed to install wave subdevice: 0x%lx\n", Status);
        goto cleanup;
    }

    /* connect the wave bridge pin to the topology source pin */
    Status = PcRegisterPhysicalConnection(DeviceObject,
                                          UnknownWave,
                                          WAVE_PIN_RENDER_BRIDGE,
                                          UnknownTopology,
                                          TOPO_PIN_WAVEOUT_SOURCE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("HDAUDIO: failed to register physical connection: 0x%lx\n", Status);
        goto cleanup;
    }

    DPRINT1("HDAUDIO: adapter started successfully\n");

cleanup:
    if (UnknownWave)
        UnknownWave->Release();
    if (UnknownTopology)
        UnknownTopology->Release();
    if (Codec)
        Codec->Release();
    if (UnknownCodec)
        UnknownCodec->Release();

    return Status;
}

/*****************************************************************************
 * AddDevice
 */
static
NTSTATUS
NTAPI
AddDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject)
{
    PAGED_CODE();

    return PcAddAdapterDevice(DriverObject,
                              PhysicalDeviceObject,
                              (PCPFNSTARTDEVICE)StartDevice,
                              MAX_MINIPORTS,
                              0);
}

/*****************************************************************************
 * DriverEntry
 */
extern "C"
NTSTATUS
NTAPI
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPathName)
{
    PAGED_CODE();

    DPRINT1("HDAUDIO: DriverEntry\n");

    return PcInitializeAdapterDriver(DriverObject,
                                     RegistryPathName,
                                     (PDRIVER_ADD_DEVICE)AddDevice);
}
