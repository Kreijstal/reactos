/*
 * PROJECT:     ReactOS Network Adapter Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry, class library registration and client binding
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * netadaptercx.sys is a KMDF class extension. Its DriverEntry registers it
 * with WDFLDR as the class library "NetAdapterCx" 2.1. A client links the
 * NetAdapterCx stub, whose class bind record in .kmdfclassbind makes the
 * KMDF client stub call WdfVersionBindClass before the client's DriverEntry.
 * WDFLDR then loads this driver (HKLM\SYSTEM\CurrentControlSet\Control\Wdf\
 * Kmdf\NetAdapterCx\Versions\2\1, value Service) and calls NxBindClient,
 * which fills the client's NetFunctions[] and NetDriverGlobals.
 *
 * The slot order of NetFunctions[] is the client ABI (netfuncenum.h): 2.0
 * clients bind the first 75 slots, 2.1 clients all 122.
 */

#include "nxprivate.h"
#include "nxapi.h"

#define NDEBUG
#include <debug.h>

NDIS_HANDLE NxNdisCxHandle;

static NETFUNC NxFunctionTable[NetFunctionTableNumEntries];

#ifdef __GNUC__
#define NX_UNUSED __attribute__((unused))
#else
#define NX_UNUSED
#endif

/* An implemented entry point must have the prototype the client calls */
#define NX_CHECK_SIGNATURE(_Name, _Pfn) \
    static _Pfn const NxSignature_##_Name NX_UNUSED = imp_##_Name

NX_CHECK_SIGNATURE(NetAdapterInitAllocate, PFN_NETADAPTERINITALLOCATE);
NX_CHECK_SIGNATURE(NetAdapterInitFree, PFN_NETADAPTERINITFREE);
NX_CHECK_SIGNATURE(NetAdapterInitSetDatapathCallbacks, PFN_NETADAPTERINITSETDATAPATHCALLBACKS);
NX_CHECK_SIGNATURE(NetAdapterCreate, PFN_NETADAPTERCREATE);
NX_CHECK_SIGNATURE(NetAdapterStart, PFN_NETADAPTERSTART);
NX_CHECK_SIGNATURE(NetAdapterStop, PFN_NETADAPTERSTOP);
NX_CHECK_SIGNATURE(NetAdapterSetLinkLayerCapabilities, PFN_NETADAPTERSETLINKLAYERCAPABILITIES);
NX_CHECK_SIGNATURE(NetAdapterSetLinkLayerMtuSize, PFN_NETADAPTERSETLINKLAYERMTUSIZE);
NX_CHECK_SIGNATURE(NetAdapterSetDataPathCapabilities, PFN_NETADAPTERSETDATAPATHCAPABILITIES);
NX_CHECK_SIGNATURE(NetAdapterSetLinkState, PFN_NETADAPTERSETLINKSTATE);
NX_CHECK_SIGNATURE(NetAdapterGetNetLuid, PFN_NETADAPTERGETNETLUID);
NX_CHECK_SIGNATURE(NetAdapterSetPermanentLinkLayerAddress, PFN_NETADAPTERSETPERMANENTLINKLAYERADDRESS);
NX_CHECK_SIGNATURE(NetAdapterSetCurrentLinkLayerAddress, PFN_NETADAPTERSETCURRENTLINKLAYERADDRESS);
NX_CHECK_SIGNATURE(NetAdapterOffloadSetChecksumCapabilities, PFN_NETADAPTEROFFLOADSETCHECKSUMCAPABILITIES);
NX_CHECK_SIGNATURE(NetOffloadIsChecksumIPv4Enabled, PFN_NETOFFLOADISCHECKSUMIPV4ENABLED);
NX_CHECK_SIGNATURE(NetOffloadIsChecksumTcpEnabled, PFN_NETOFFLOADISCHECKSUMTCPENABLED);
NX_CHECK_SIGNATURE(NetOffloadIsChecksumUdpEnabled, PFN_NETOFFLOADISCHECKSUMUDPENABLED);
NX_CHECK_SIGNATURE(NetDeviceInitConfig, PFN_NETDEVICEINITCONFIG);
NX_CHECK_SIGNATURE(NetRxQueueCreate, PFN_NETRXQUEUECREATE);
NX_CHECK_SIGNATURE(NetRxQueueNotifyMoreReceivedPacketsAvailable, PFN_NETRXQUEUENOTIFYMORERECEIVEDPACKETSAVAILABLE);
NX_CHECK_SIGNATURE(NetRxQueueInitGetQueueId, PFN_NETRXQUEUEINITGETQUEUEID);
NX_CHECK_SIGNATURE(NetRxQueueGetRingCollection, PFN_NETRXQUEUEGETRINGCOLLECTION);
NX_CHECK_SIGNATURE(NetRxQueueGetExtension, PFN_NETRXQUEUEGETEXTENSION);
NX_CHECK_SIGNATURE(NetTxQueueCreate, PFN_NETTXQUEUECREATE);
NX_CHECK_SIGNATURE(NetTxQueueNotifyMoreCompletedPacketsAvailable, PFN_NETTXQUEUENOTIFYMORECOMPLETEDPACKETSAVAILABLE);
NX_CHECK_SIGNATURE(NetTxQueueInitGetQueueId, PFN_NETTXQUEUEINITGETQUEUEID);
NX_CHECK_SIGNATURE(NetTxQueueGetRingCollection, PFN_NETTXQUEUEGETRINGCOLLECTION);
NX_CHECK_SIGNATURE(NetTxQueueGetExtension, PFN_NETTXQUEUEGETEXTENSION);
NX_CHECK_SIGNATURE(NetAdapterOffloadSetTxChecksumCapabilities, PFN_NETADAPTEROFFLOADSETTXCHECKSUMCAPABILITIES);
NX_CHECK_SIGNATURE(NetOffloadIsTxChecksumIPv4Enabled, PFN_NETOFFLOADISTXCHECKSUMIPV4ENABLED);
NX_CHECK_SIGNATURE(NetOffloadIsTxChecksumTcpEnabled, PFN_NETOFFLOADISTXCHECKSUMTCPENABLED);
NX_CHECK_SIGNATURE(NetOffloadIsTxChecksumUdpEnabled, PFN_NETOFFLOADISTXCHECKSUMUDPENABLED);
NX_CHECK_SIGNATURE(NetAdapterOffloadSetRxChecksumCapabilities, PFN_NETADAPTEROFFLOADSETRXCHECKSUMCAPABILITIES);
NX_CHECK_SIGNATURE(NetOffloadIsRxChecksumIPv4Enabled, PFN_NETOFFLOADISRXCHECKSUMIPV4ENABLED);
NX_CHECK_SIGNATURE(NetOffloadIsRxChecksumTcpEnabled, PFN_NETOFFLOADISRXCHECKSUMTCPENABLED);
NX_CHECK_SIGNATURE(NetOffloadIsRxChecksumUdpEnabled, PFN_NETOFFLOADISRXCHECKSUMUDPENABLED);
NX_CHECK_SIGNATURE(NetTxQueueGetDemux8021p, PFN_NETTXQUEUEGETDEMUX8021P);

VOID
NxFillFunctionTable(
    _Out_writes_(NetFunctionTableNumEntries) NETFUNC *Table)
{
    static const NETFUNC Implemented[NetFunctionTableNumEntries] =
    {
        [NetAdapterInitAllocateTableIndex] = (NETFUNC)imp_NetAdapterInitAllocate,
        [NetAdapterInitFreeTableIndex] = (NETFUNC)imp_NetAdapterInitFree,
        [NetAdapterInitSetDatapathCallbacksTableIndex] = (NETFUNC)imp_NetAdapterInitSetDatapathCallbacks,
        [NetAdapterCreateTableIndex] = (NETFUNC)imp_NetAdapterCreate,
        [NetAdapterStartTableIndex] = (NETFUNC)imp_NetAdapterStart,
        [NetAdapterStopTableIndex] = (NETFUNC)imp_NetAdapterStop,
        [NetAdapterSetLinkLayerCapabilitiesTableIndex] = (NETFUNC)imp_NetAdapterSetLinkLayerCapabilities,
        [NetAdapterSetLinkLayerMtuSizeTableIndex] = (NETFUNC)imp_NetAdapterSetLinkLayerMtuSize,
        [NetAdapterSetDataPathCapabilitiesTableIndex] = (NETFUNC)imp_NetAdapterSetDataPathCapabilities,
        [NetAdapterSetLinkStateTableIndex] = (NETFUNC)imp_NetAdapterSetLinkState,
        [NetAdapterGetNetLuidTableIndex] = (NETFUNC)imp_NetAdapterGetNetLuid,
        [NetAdapterSetPermanentLinkLayerAddressTableIndex] = (NETFUNC)imp_NetAdapterSetPermanentLinkLayerAddress,
        [NetAdapterSetCurrentLinkLayerAddressTableIndex] = (NETFUNC)imp_NetAdapterSetCurrentLinkLayerAddress,
        [NetAdapterOffloadSetChecksumCapabilitiesTableIndex] = (NETFUNC)imp_NetAdapterOffloadSetChecksumCapabilities,
        [NetOffloadIsChecksumIPv4EnabledTableIndex] = (NETFUNC)imp_NetOffloadIsChecksumIPv4Enabled,
        [NetOffloadIsChecksumTcpEnabledTableIndex] = (NETFUNC)imp_NetOffloadIsChecksumTcpEnabled,
        [NetOffloadIsChecksumUdpEnabledTableIndex] = (NETFUNC)imp_NetOffloadIsChecksumUdpEnabled,
        [NetAdapterInitGetCreatedAdapterTableIndex] = (NETFUNC)imp_NetAdapterInitGetCreatedAdapter,
        [NetAdapterGetParentTableIndex] = (NETFUNC)imp_NetAdapterGetParent,
        [NetAdapterGetLinkLayerMtuSizeTableIndex] = (NETFUNC)imp_NetAdapterGetLinkLayerMtuSize,
        [NetAdapterWdmGetNdisHandleTableIndex] = (NETFUNC)imp_NetAdapterWdmGetNdisHandle,
        [NetAdapterDriverWdmGetHandleTableIndex] = (NETFUNC)imp_NetAdapterDriverWdmGetHandle,
        [NetDeviceInitConfigTableIndex] = (NETFUNC)imp_NetDeviceInitConfig,
        [NetRxQueueCreateTableIndex] = (NETFUNC)imp_NetRxQueueCreate,
        [NetRxQueueNotifyMoreReceivedPacketsAvailableTableIndex] = (NETFUNC)imp_NetRxQueueNotifyMoreReceivedPacketsAvailable,
        [NetRxQueueInitGetQueueIdTableIndex] = (NETFUNC)imp_NetRxQueueInitGetQueueId,
        [NetRxQueueGetRingCollectionTableIndex] = (NETFUNC)imp_NetRxQueueGetRingCollection,
        [NetRxQueueGetExtensionTableIndex] = (NETFUNC)imp_NetRxQueueGetExtension,
        [NetTxQueueCreateTableIndex] = (NETFUNC)imp_NetTxQueueCreate,
        [NetTxQueueNotifyMoreCompletedPacketsAvailableTableIndex] = (NETFUNC)imp_NetTxQueueNotifyMoreCompletedPacketsAvailable,
        [NetTxQueueInitGetQueueIdTableIndex] = (NETFUNC)imp_NetTxQueueInitGetQueueId,
        [NetTxQueueGetRingCollectionTableIndex] = (NETFUNC)imp_NetTxQueueGetRingCollection,
        [NetTxQueueGetExtensionTableIndex] = (NETFUNC)imp_NetTxQueueGetExtension,
        [NetAdapterOffloadSetTxChecksumCapabilitiesTableIndex] = (NETFUNC)imp_NetAdapterOffloadSetTxChecksumCapabilities,
        [NetOffloadIsTxChecksumIPv4EnabledTableIndex] = (NETFUNC)imp_NetOffloadIsTxChecksumIPv4Enabled,
        [NetOffloadIsTxChecksumTcpEnabledTableIndex] = (NETFUNC)imp_NetOffloadIsTxChecksumTcpEnabled,
        [NetOffloadIsTxChecksumUdpEnabledTableIndex] = (NETFUNC)imp_NetOffloadIsTxChecksumUdpEnabled,
        [NetAdapterOffloadSetRxChecksumCapabilitiesTableIndex] = (NETFUNC)imp_NetAdapterOffloadSetRxChecksumCapabilities,
        [NetOffloadIsRxChecksumIPv4EnabledTableIndex] = (NETFUNC)imp_NetOffloadIsRxChecksumIPv4Enabled,
        [NetOffloadIsRxChecksumTcpEnabledTableIndex] = (NETFUNC)imp_NetOffloadIsRxChecksumTcpEnabled,
        [NetOffloadIsRxChecksumUdpEnabledTableIndex] = (NETFUNC)imp_NetOffloadIsRxChecksumUdpEnabled,
        [NetTxQueueGetDemux8021pTableIndex] = (NETFUNC)imp_NetTxQueueGetDemux8021p,
    };
    ULONG i;

    RtlZeroMemory(Table, NetFunctionTableNumEntries * sizeof(NETFUNC));
    NxFillStubSlots(Table);

    for (i = 0; i < NetFunctionTableNumEntries; i++)
    {
        if (Implemented[i] != NULL)
            Table[i] = Implemented[i];

        /* Never hand out an empty slot */
        ASSERT(Table[i] != NULL);
    }
}

NTSTATUS
NxGetMiniportDriverHandle(
    _In_ PNX_PRIVATE_GLOBALS Globals,
    _In_ PDRIVER_OBJECT ClientDriverObject,
    _Out_ PNDIS_HANDLE MiniportDriverHandle)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS Characteristics;
    NDIS_STATUS Status = NDIS_STATUS_SUCCESS;

    PAGED_CODE();

    *MiniportDriverHandle = NULL;

    ExAcquireFastMutex(&Globals->Lock);

    if (Globals->MiniportDriverHandle == NULL)
    {
        NxMiniportFillCharacteristics(&Characteristics);

        Status = NdisWdfRegisterMiniportDriver(NxNdisCxHandle,
                                               ClientDriverObject,
                                               NULL,
                                               Globals,
                                               &Characteristics,
                                               &Globals->MiniportDriverHandle);
        if (Status == NDIS_STATUS_SUCCESS)
            Globals->ClientDriverObject = ClientDriverObject;
        else
            DPRINT1("NdisWdfRegisterMiniportDriver failed 0x%lx\n", Status);
    }

    *MiniportDriverHandle = Globals->MiniportDriverHandle;

    ExReleaseFastMutex(&Globals->Lock);

    return (Status == NDIS_STATUS_SUCCESS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
NxInitialize(VOID)
{
    NxFillFunctionTable(NxFunctionTable);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
NxDeinitialize(VOID)
{
}

static
NTSTATUS
NTAPI
NxBindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _Out_ PWDF_COMPONENT_GLOBALS *ComponentGlobals)
{
    PNET_DRIVER_GLOBALS *ClientGlobals;
    PNX_PRIVATE_GLOBALS Globals;
    ULONG Minor, Expected;

    PAGED_CODE();

    ClientGlobals = (PNET_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo;
    if (ClientGlobals == NULL || ClassBindInfo->FunctionTable == NULL)
        return STATUS_INVALID_PARAMETER;

    /* KMDF unbinds even after a failed bind */
    *ClientGlobals = NULL;

    Minor = ClassBindInfo->Version.Minor;

    if (ClassBindInfo->Version.Major != NX_VERSION_MAJOR ||
        Minor > NX_VERSION_MINOR ||
        ClassBindInfo->Version.Build != 0)
    {
        /* 1.x and the 2.x previews (Build > 0) have another ABI */
        DPRINT1("NetAdapterCx %lu.%lu.%lu client, this is %u.%u\n",
                ClassBindInfo->Version.Major, Minor, ClassBindInfo->Version.Build,
                NX_VERSION_MAJOR, NX_VERSION_MINOR);
        return STATUS_NOT_SUPPORTED;
    }

    Expected = (Minor == 0) ? NX_TABLE_ENTRIES_2_0 : NX_TABLE_ENTRIES_2_1;
    if (ClassBindInfo->FunctionTableCount != Expected)
    {
        DPRINT1("NetAdapterCx 2.%lu client has %lu table slots, expected %lu\n",
                Minor, ClassBindInfo->FunctionTableCount, Expected);
        return STATUS_INVALID_PARAMETER;
    }

    Globals = ExAllocatePoolZero(NonPagedPool, sizeof(*Globals), NX_TAG);
    if (Globals == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Globals->Signature = NX_GLOBALS_SIGNATURE;
    /* What the KMDF client stub passes is its WdfDriverGlobals pointer */
    Globals->ClientDriverGlobals = (PWDF_DRIVER_GLOBALS)ComponentGlobals;
    Globals->ClientMinor = Minor;
    ExInitializeFastMutex(&Globals->Lock);

    RtlCopyMemory(ClassBindInfo->FunctionTable,
                  NxFunctionTable,
                  Expected * sizeof(NxFunctionTable[0]));

    *ClientGlobals = &Globals->Public;

    DbgPrint("NetAdapterCx: bound 2.%lu client (%lu functions)\n", Minor, Expected);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
NxUnbindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS *ComponentGlobals)
{
    PNET_DRIVER_GLOBALS *ClientGlobals;
    PNX_PRIVATE_GLOBALS Globals;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(ComponentGlobals);

    ClientGlobals = (PNET_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo;
    if (ClientGlobals == NULL || *ClientGlobals == NULL)
        return;

    Globals = NxGetPrivateGlobals(*ClientGlobals);
    if (Globals->MiniportDriverHandle != NULL)
        NdisWdfDeregisterMiniportDriver(Globals->MiniportDriverHandle);

    Globals->Signature = 0;
    ExFreePoolWithTag(Globals, NX_TAG);
    *ClientGlobals = NULL;
}

static WDF_CLASS_LIBRARY_INFO NxClassLibraryInfo =
{
    sizeof(WDF_CLASS_LIBRARY_INFO),
    { NX_VERSION_MAJOR, NX_VERSION_MINOR, 0 },
    NxInitialize,
    NxDeinitialize,
    NxBindClient,
    NxUnbindClient
};

static EVT_WDF_DRIVER_UNLOAD NxEvtDriverUnload;

static
VOID
NTAPI
NxEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);

    if (NxNdisCxHandle != NULL)
    {
        NdisWdfDeregisterCx(NxNdisCxHandle);
        NxNdisCxHandle = NULL;
    }
}

DRIVER_INITIALIZE DriverEntry;

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NDIS_WDF_CX_CHARACTERISTICS CxCharacteristics;
    WDF_DRIVER_CONFIG Config;
    NDIS_STATUS NdisStatus;
    NTSTATUS Status;

    NxDeviceInitializeList();

    WDF_DRIVER_CONFIG_INIT(&Config, WDF_NO_EVENT_CALLBACK);
    Config.DriverInitFlags |= WdfDriverInitNonPnpDriver;
    Config.EvtDriverUnload = NxEvtDriverUnload;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                             &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WdfDriverCreate failed 0x%lx\n", Status);
        return Status;
    }

    RtlZeroMemory(&CxCharacteristics, sizeof(CxCharacteristics));
    CxCharacteristics.Size = sizeof(CxCharacteristics);
    CxCharacteristics.Version = NDIS_WDF_CX_CHARACTERISTICS_VERSION_1;
    CxCharacteristics.CxDriverObject = DriverObject;

    NdisStatus = NdisWdfRegisterCx(&CxCharacteristics, &NxNdisCxHandle);
    if (NdisStatus != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("NdisWdfRegisterCx failed 0x%lx\n", NdisStatus);
        return STATUS_UNSUCCESSFUL;
    }

    Status = WdfRegisterClassLibrary(&NxClassLibraryInfo, RegistryPath, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WdfRegisterClassLibrary failed 0x%lx\n", Status);
        NdisWdfDeregisterCx(NxNdisCxHandle);
        NxNdisCxHandle = NULL;
        return Status;
    }

    DbgPrint("NetAdapterCx: class extension %u.%u registered\n",
             NX_VERSION_MAJOR, NX_VERSION_MINOR);
    return STATUS_SUCCESS;
}
