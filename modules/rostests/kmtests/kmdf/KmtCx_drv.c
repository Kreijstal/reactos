/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test KMDF class extension
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include <ntddk.h>
#include <wdf.h>
#include <wdfldr.h>

#include "KmtCx.h"

#define TAG_KMTCX 'xCtK'

static PFN_KMTCXFUNC KmtCxExportTable[KmtCxFunctionTableNumEntries];
static ULONG KmtCxInitializeCount;
static ULONG KmtCxBindCount;

static
ULONG
NTAPI
KmtCxGetMagic(
    _In_ PKMTCX_DRIVER_GLOBALS Globals)
{
    return (Globals && Globals->Size == sizeof(*Globals)) ? KMTCX_MAGIC : 0;
}

static
ULONG
NTAPI
KmtCxAdd(
    _In_ PKMTCX_DRIVER_GLOBALS Globals,
    _In_ ULONG A,
    _In_ ULONG B)
{
    UNREFERENCED_PARAMETER(Globals);
    return A + B;
}

static
VOID
NTAPI
KmtCxGetLibraryState(
    _In_ PKMTCX_DRIVER_GLOBALS Globals,
    _Out_ PULONG InitializeCount,
    _Out_ PULONG BindCount)
{
    UNREFERENCED_PARAMETER(Globals);
    *InitializeCount = KmtCxInitializeCount;
    *BindCount = KmtCxBindCount;
}

static
NTSTATUS
NTAPI
KmtCxInitialize(VOID)
{
    KmtCxExportTable[KmtCxGetMagicTableIndex] = (PFN_KMTCXFUNC)KmtCxGetMagic;
    KmtCxExportTable[KmtCxAddTableIndex] = (PFN_KMTCXFUNC)KmtCxAdd;
    KmtCxExportTable[KmtCxGetLibraryStateTableIndex] = (PFN_KMTCXFUNC)KmtCxGetLibraryState;
    KmtCxInitializeCount++;
    DbgPrint("KmtCx: class library initialized\n");
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
KmtCxDeinitialize(VOID)
{
    DbgPrint("KmtCx: class library deinitialized\n");
}

static
NTSTATUS
NTAPI
KmtCxBindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _Out_ PWDF_COMPONENT_GLOBALS *ComponentGlobals)
{
    PKMTCX_DRIVER_GLOBALS *ClientGlobals;
    PKMTCX_DRIVER_GLOBALS Globals;

    ClientGlobals = (PKMTCX_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo;
    if (ClientGlobals == NULL)
        return STATUS_INVALID_PARAMETER;
    *ClientGlobals = NULL;

    if (ClassBindInfo->Size != sizeof(WDF_CLASS_BIND_INFO) &&
        ClassBindInfo->Size != sizeof(WDF_CLASS_BIND_INFO2))
    {
        DbgPrint("KmtCx: bad class bind info size %lu\n", ClassBindInfo->Size);
        return STATUS_INVALID_PARAMETER;
    }

    if (ClassBindInfo->Version.Major != KMTCX_MAJOR_VERSION ||
        ClassBindInfo->Version.Minor > KMTCX_MINOR_VERSION ||
        ClassBindInfo->FunctionTableCount != KmtCxFunctionTableNumEntries)
    {
        DbgPrint("KmtCx: client wants %lu.%lu with %lu functions\n",
                 ClassBindInfo->Version.Major, ClassBindInfo->Version.Minor,
                 ClassBindInfo->FunctionTableCount);
        return STATUS_REVISION_MISMATCH;
    }

    Globals = ExAllocatePoolZero(NonPagedPool, sizeof(*Globals), TAG_KMTCX);
    if (Globals == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Globals->Size = sizeof(*Globals);
    Globals->ClientGlobals = (PVOID)ComponentGlobals;
    Globals->BindInfoSize = ClassBindInfo->Size;
    Globals->BindCount = ++KmtCxBindCount;

    RtlCopyMemory(ClassBindInfo->FunctionTable,
                  KmtCxExportTable,
                  sizeof(KmtCxExportTable));
    *ClientGlobals = Globals;

    DbgPrint("KmtCx: bound client %lu, class bind info size 0x%lx, client globals %p\n",
             Globals->BindCount, ClassBindInfo->Size, ComponentGlobals);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
KmtCxUnbindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS *ComponentGlobals)
{
    PKMTCX_DRIVER_GLOBALS *ClientGlobals;

    UNREFERENCED_PARAMETER(ComponentGlobals);

    ClientGlobals = (PKMTCX_DRIVER_GLOBALS *)ClassBindInfo->ClassBindInfo;
    if (ClientGlobals == NULL || *ClientGlobals == NULL)
        return;

    DbgPrint("KmtCx: unbound client %lu\n", (*ClientGlobals)->BindCount);
    ExFreePoolWithTag(*ClientGlobals, TAG_KMTCX);
    *ClientGlobals = NULL;
}

static WDF_CLASS_LIBRARY_INFO KmtCxClassLibraryInfo =
{
    sizeof(WDF_CLASS_LIBRARY_INFO),
    { KMTCX_MAJOR_VERSION, KMTCX_MINOR_VERSION, 0 },
    KmtCxInitialize,
    KmtCxDeinitialize,
    KmtCxBindClient,
    KmtCxUnbindClient
};

static EVT_WDF_DRIVER_UNLOAD KmtCxEvtDriverUnload;

static
VOID
NTAPI
KmtCxEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    DbgPrint("KmtCx: unloading\n");
}

DRIVER_INITIALIZE DriverEntry;

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    WDF_DRIVER_CONFIG_INIT(&Config, WDF_NO_EVENT_CALLBACK);
    Config.DriverInitFlags |= WdfDriverInitNonPnpDriver;
    Config.EvtDriverUnload = KmtCxEvtDriverUnload;

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("KmtCx: WdfDriverCreate failed 0x%lx\n", Status);
        return Status;
    }

    Status = WdfRegisterClassLibrary(&KmtCxClassLibraryInfo, RegistryPath, NULL);
    DbgPrint("KmtCx: WdfRegisterClassLibrary(%wZ) returned 0x%lx\n", RegistryPath, Status);
    return Status;
}
