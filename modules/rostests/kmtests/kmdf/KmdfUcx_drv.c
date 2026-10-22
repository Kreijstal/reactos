/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     UCX class extension binding test driver
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

/*
 * Binds to ucx01000 the way a WDK-built UCX client does: through a
 * WDF_CLASS_BIND_INFO in .kmdfclassbind, before DriverEntry runs.
 */

#include <kmt_test.h>
#include <wdf.h>
#include <fxldr.h>
#include <ucxclass.h>

#include "KmdfUcx.h"

#define UCX_TEST_MAJOR_VERSION 1
#define UCX_TEST_MINOR_VERSION 6

PFN_UCXFUNC UcxFunctions[UcxFunctionTableNumEntries];
PUCX_DRIVER_GLOBALS UcxDriverGlobals;

WDF_CLASS_BIND_INFO_ALLOCATE
WDF_CLASS_BIND_INFO UcxBindInfo =
{
    sizeof(WDF_CLASS_BIND_INFO),
    (PWCHAR)L"Ucx",
    { UCX_TEST_MAJOR_VERSION, UCX_TEST_MINOR_VERSION, 0 },
    (VOID (NTAPI **)(VOID))UcxFunctions,
    UcxFunctionTableNumEntries,
    &UcxDriverGlobals,
    NULL,
    NULL,
    NULL
};

static PDRIVER_UNLOAD WdfDriverUnloadRoutine;

static
VOID
NTAPI
KmdfUcxEvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
}

static
NTSTATUS
TestMessageHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _In_opt_ PVOID Buffer,
    _In_ SIZE_T InLength,
    _Inout_ PSIZE_T OutLength)
{
    ULONG i, Missing = 0;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);
    UNREFERENCED_PARAMETER(OutLength);

    if (ControlCode != IOCTL_KMDFUCX_RUN_TESTS)
        return STATUS_NOT_SUPPORTED;

    ok(UcxDriverGlobals != NULL, "UCX not bound before DriverEntry\n");
    ok(UcxBindInfo.ClassModule != NULL, "ClassModule not set by the loader\n");
    for (i = 0; i < UcxFunctionTableNumEntries; i++)
    {
        if (UcxFunctions[i] == NULL)
            Missing++;
    }
    ok_eq_ulong(Missing, 0UL);
    ok(UcxFunctions[UcxControllerCreateTableIndex] != NULL, "No UcxControllerCreate\n");
    trace("UCX globals %p, UcxControllerCreate %p\n",
          UcxDriverGlobals, UcxFunctions[UcxControllerCreateTableIndex]);
    return STATUS_SUCCESS;
}

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(Flags);

    *DeviceName = L"KmdfUcx";

    WDF_DRIVER_CONFIG_INIT(&Config, WDF_NO_EVENT_CALLBACK);
    Config.DriverInitFlags |= WdfDriverInitNonPnpDriver | WdfDriverInitNoDispatchOverride;
    Config.EvtDriverUnload = KmdfUcxEvtDriverUnload;

    Status = WdfDriverCreate(DriverObject, (PUNICODE_STRING)RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
        return Status;

    WdfDriverUnloadRoutine = DriverObject->DriverUnload;

    return KmtRegisterMessageHandler(IOCTL_KMDFUCX_RUN_TESTS, NULL, TestMessageHandler);
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    if (WdfDriverUnloadRoutine)
        WdfDriverUnloadRoutine(DriverObject);
}
