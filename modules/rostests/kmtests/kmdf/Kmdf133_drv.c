/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KMDF 1.33 client test driver
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

/*
 * A GCC-built KMDF 1.33 client. It checks the handshake done by the client
 * stub (WDF_BIND_INFO2 and the class extension bind table), that the test
 * class extension got bound before DriverEntry, and exercises a range of
 * framework objects through the 1.33 function table.
 */

#include <kmt_test.h>
#include <wdf.h>
#include <fxldr.h>

#include "Kmdf133.h"
#include "KmtCx.h"

#define NDEBUG
#include <debug.h>

/* Provided by the client stub (wdfdriverentry) */
extern WDF_BIND_INFO2 WdfBindInfo;
extern ULONG WdfFunctionCount;
extern ULONG WdfStructureCount;
extern BOOLEAN WdfClientVersionHigherThanFramework;
extern PVOID __KMDF_CLASS_BIND_START;
extern PVOID __KMDF_CLASS_BIND_END;

/* Bind to the test class extension through .kmdfclassbind */
PFN_KMTCXFUNC KmtCxFunctions[KmtCxFunctionTableNumEntries];
PKMTCX_DRIVER_GLOBALS KmtCxDriverGlobals;

WDF_CLASS_BIND_INFO_ALLOCATE
WDF_CLASS_BIND_INFO KmtCxBindInfo =
{
    sizeof(WDF_CLASS_BIND_INFO),
    (PWCHAR)KMTCX_CLASS_NAME,
    { KMTCX_MAJOR_VERSION, KMTCX_MINOR_VERSION, 0 },
    (VOID (NTAPI **)(VOID))KmtCxFunctions,
    KmtCxFunctionTableNumEntries,
    &KmtCxDriverGlobals,
    NULL,
    NULL,
    NULL
};

typedef struct _KMDF133_CONTEXT
{
    ULONG Magic;
    LONG CleanupCount;
} KMDF133_CONTEXT, *PKMDF133_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(KMDF133_CONTEXT, Kmdf133GetContext)

static PDRIVER_UNLOAD WdfDriverUnloadRoutine;
static LONG CleanupCallbacks;
static KEVENT TimerEvent;
static LONG WorkItemRuns;

static
VOID
NTAPI
Kmdf133EvtCleanup(
    _In_ WDFOBJECT Object)
{
    Kmdf133GetContext(Object)->CleanupCount++;
    InterlockedIncrement(&CleanupCallbacks);
}

static
VOID
NTAPI
Kmdf133EvtDriverUnload(
    _In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    DbgPrint("Kmdf133: EvtDriverUnload\n");
}

static
VOID
NTAPI
Kmdf133EvtTimer(
    _In_ WDFTIMER Timer)
{
    UNREFERENCED_PARAMETER(Timer);
    KeSetEvent(&TimerEvent, IO_NO_INCREMENT, FALSE);
}

static
VOID
NTAPI
Kmdf133EvtWorkItem(
    _In_ WDFWORKITEM WorkItem)
{
    UNREFERENCED_PARAMETER(WorkItem);
    InterlockedIncrement(&WorkItemRuns);
}

static
VOID
TestStub(VOID)
{
    PUCHAR Start = (PUCHAR)&__KMDF_CLASS_BIND_START;
    PUCHAR End = (PUCHAR)&__KMDF_CLASS_BIND_END;
    PUCHAR Entry = (PUCHAR)&KmtCxBindInfo;

    ok(WdfDriverGlobals != NULL, "WdfDriverGlobals is NULL\n");
    ok(WdfFunctions != NULL, "WdfFunctions is NULL\n");
    ok_eq_ulong(WdfBindInfo.V1.Size, (ULONG)sizeof(WDF_BIND_INFO2));
    ok_eq_ulong(WdfBindInfo.V1.Version.Major, 1UL);
    ok_eq_ulong(WdfBindInfo.V1.Version.Minor, 33UL);
    ok_eq_ulong(WdfBindInfo.V1.FuncCount, 458UL);
    ok_eq_ulong(WdfFunctionCount, 458UL);
    ok_eq_ulong(WdfStructureCount, 81UL);
    ok_eq_bool(WdfClientVersionHigherThanFramework, FALSE);
#ifdef _WIN64
    ok_eq_size(sizeof(WDF_BIND_INFO2), (SIZE_T)0x58);
    ok_eq_size(sizeof(WDF_CLASS_BIND_INFO), (SIZE_T)0x50);
    ok_eq_size(sizeof(WDF_CLASS_BIND_INFO2), (SIZE_T)0x78);
#endif

    /* The linker must have put our entry between the stub's markers */
    ok(Start + 16 <= Entry && Entry + sizeof(KmtCxBindInfo) <= End,
       "Class bind entry %p outside of [%p, %p)\n", Entry, Start, End);
    trace("class bind section: start %p entry %p end %p\n", Start, Entry, End);
}

static
VOID
TestClassExtension(VOID)
{
    ULONG InitializeCount = 0, BindCount = 0;

    ok(KmtCxDriverGlobals != NULL, "Class extension not bound before DriverEntry\n");
    if (!KmtCxDriverGlobals)
        return;

    ok(KmtCxBindInfo.ClassModule != NULL, "ClassModule not set by the loader\n");
    ok_eq_ulong(KmtCxDriverGlobals->Size, (ULONG)sizeof(KMTCX_DRIVER_GLOBALS));
    ok_eq_ulong(KmtCxDriverGlobals->BindInfoSize, (ULONG)sizeof(WDF_CLASS_BIND_INFO));
    ok_eq_pointer(KmtCxDriverGlobals->ClientGlobals, WdfDriverGlobals);

    ok_eq_hex(((PFN_KMTCXGETMAGIC)KmtCxFunctions[KmtCxGetMagicTableIndex])(KmtCxDriverGlobals),
              (ULONG)KMTCX_MAGIC);
    ok_eq_ulong(((PFN_KMTCXADD)KmtCxFunctions[KmtCxAddTableIndex])(KmtCxDriverGlobals, 40, 2), 42UL);

    ((PFN_KMTCXGETLIBRARYSTATE)KmtCxFunctions[KmtCxGetLibraryStateTableIndex])(KmtCxDriverGlobals,
                                                                              &InitializeCount,
                                                                              &BindCount);
    ok_eq_ulong(InitializeCount, 1UL);
    ok_eq_ulong(BindCount, KmtCxDriverGlobals->BindCount);
    trace("KmtCx initialized %lu time(s), %lu bind(s)\n", InitializeCount, BindCount);
}

static
VOID
TestVersion(VOID)
{
    WDF_DRIVER_VERSION_AVAILABLE_PARAMS Params;
    WDFSTRING String;
    UNICODE_STRING Version;
    NTSTATUS Status;

    WDF_DRIVER_VERSION_AVAILABLE_PARAMS_INIT(&Params, 1, 33);
    ok_eq_bool(WdfDriverIsVersionAvailable(WdfGetDriver(), &Params), TRUE);
    WDF_DRIVER_VERSION_AVAILABLE_PARAMS_INIT(&Params, 1, 17);
    ok_eq_bool(WdfDriverIsVersionAvailable(WdfGetDriver(), &Params), TRUE);
    WDF_DRIVER_VERSION_AVAILABLE_PARAMS_INIT(&Params, 1, 35);
    ok_eq_bool(WdfDriverIsVersionAvailable(WdfGetDriver(), &Params), FALSE);

    Status = WdfStringCreate(NULL, WDF_NO_OBJECT_ATTRIBUTES, &String);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = WdfDriverRetrieveVersionString(WdfGetDriver(), String);
    ok_eq_hex(Status, STATUS_SUCCESS);
    WdfStringGetUnicodeString(String, &Version);
    ok(Version.Length > 0, "Empty version string\n");
    trace("Framework version string: %wZ\n", &Version);
    WdfObjectDelete(String);
}

static
VOID
TestObjects(VOID)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFOBJECT Object;
    WDFCOLLECTION Collection;
    WDFMEMORY Memory;
    WDFSPINLOCK SpinLock;
    WDFWAITLOCK WaitLock;
    WDFLOOKASIDE Lookaside;
    WDFMEMORY LookasideMemory;
    WDFREQUEST Request;
    PKMDF133_CONTEXT Context;
    PVOID Buffer;
    size_t Size;
    UCHAR Data[16];
    NTSTATUS Status;
    ULONG i;

    /* Object with a typed context and a cleanup callback */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, KMDF133_CONTEXT);
    Attributes.EvtCleanupCallback = Kmdf133EvtCleanup;
    Status = WdfObjectCreate(&Attributes, &Object);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Context = Kmdf133GetContext(Object);
        ok(Context != NULL, "No context\n");
        ok_eq_ulong(Context->Magic, 0UL);
        Context->Magic = 0x133;
        ok_eq_ulong(Kmdf133GetContext(Object)->Magic, 0x133UL);
        ok_eq_pointer(WdfObjectGetTypedContext(Object, KMDF133_CONTEXT), Context);

        /* Collection holding the object */
        Status = WdfCollectionCreate(WDF_NO_OBJECT_ATTRIBUTES, &Collection);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            Status = WdfCollectionAdd(Collection, Object);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok_eq_ulong(WdfCollectionGetCount(Collection), 1UL);
            ok_eq_pointer(WdfCollectionGetItem(Collection, 0), Object);
            WdfCollectionRemove(Collection, Object);
            ok_eq_ulong(WdfCollectionGetCount(Collection), 0UL);
            WdfObjectDelete(Collection);
        }

        WdfObjectDelete(Object);
        ok_eq_long(CleanupCallbacks, 1L);
    }

    /* Memory */
    Status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES, NonPagedPool, 'tdfK', 64, &Memory, &Buffer);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_eq_pointer(WdfMemoryGetBuffer(Memory, &Size), Buffer);
        ok_eq_size(Size, (size_t)64);
        for (i = 0; i < sizeof(Data); i++)
            Data[i] = (UCHAR)i;
        Status = WdfMemoryCopyFromBuffer(Memory, 8, Data, sizeof(Data));
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(RtlCompareMemory((PUCHAR)Buffer + 8, Data, sizeof(Data)) == sizeof(Data), "Copy mismatch\n");
        Status = WdfMemoryCopyFromBuffer(Memory, 60, Data, sizeof(Data));
        ok_eq_hex(Status, STATUS_INVALID_BUFFER_SIZE);
        WdfObjectDelete(Memory);
    }

    /* Locks */
    Status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &SpinLock);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        WdfSpinLockAcquire(SpinLock);
        ok_irql(DISPATCH_LEVEL);
        WdfSpinLockRelease(SpinLock);
        ok_irql(PASSIVE_LEVEL);
        WdfObjectDelete(SpinLock);
    }

    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &WaitLock);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = WdfWaitLockAcquire(WaitLock, NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        WdfWaitLockRelease(WaitLock);
        WdfObjectDelete(WaitLock);
    }

    /* Lookaside list */
    Status = WdfLookasideListCreate(WDF_NO_OBJECT_ATTRIBUTES, 128, NonPagedPool,
                                    WDF_NO_OBJECT_ATTRIBUTES, 'ldfK', &Lookaside);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = WdfMemoryCreateFromLookaside(Lookaside, &LookasideMemory);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            WdfMemoryGetBuffer(LookasideMemory, &Size);
            ok_eq_size(Size, (size_t)128);
            WdfObjectDelete(LookasideMemory);
        }
        WdfObjectDelete(Lookaside);
    }

    /* Request without an I/O target */
    Status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, NULL, &Request);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        WdfObjectDelete(Request);
}

static
VOID
TestRegistry(VOID)
{
    WDFKEY Key;
    ULONG Value = 0;
    NTSTATUS Status;
    DECLARE_CONST_UNICODE_STRING(ValueName, L"Kmdf133Value");

    Status = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_ALL_ACCESS,
                                                WDF_NO_OBJECT_ATTRIBUTES, &Key);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = WdfRegistryAssignULong(Key, &ValueName, 0x1133);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = WdfRegistryQueryULong(Key, &ValueName, &Value);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(Value, 0x1133UL);
        WdfRegistryClose(Key);
    }

    /* KMDF 1.25 */
    Status = WdfDriverOpenPersistentStateRegistryKey(WdfGetDriver(), KEY_ALL_ACCESS,
                                                     WDF_NO_OBJECT_ATTRIBUTES, &Key);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = WdfRegistryAssignULong(Key, &ValueName, 0x2533);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Value = 0;
        Status = WdfRegistryQueryULong(Key, &ValueName, &Value);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(Value, 0x2533UL);
        WdfRegistryClose(Key);
    }
}

static
VOID
TestControlDevice(VOID)
{
    DECLARE_CONST_UNICODE_STRING(Sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    DECLARE_CONST_UNICODE_STRING(DeviceName, L"\\Device\\Kmdf133Control");
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_TIMER_CONFIG TimerConfig;
    WDF_WORKITEM_CONFIG WorkItemConfig;
    PWDFDEVICE_INIT DeviceInit;
    WDFDEVICE Device;
    WDFTIMER Timer;
    WDFWORKITEM WorkItem;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    DeviceInit = WdfControlDeviceInitAllocate(WdfGetDriver(), &Sddl);
    ok(DeviceInit != NULL, "WdfControlDeviceInitAllocate failed\n");
    if (!DeviceInit)
        return;

    Status = WdfDeviceInitAssignName(DeviceInit, &DeviceName);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &Device);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        if (DeviceInit)
            WdfDeviceInitFree(DeviceInit);
        return;
    }

    ok(WdfDeviceWdmGetDeviceObject(Device) != NULL, "No WDM device object\n");
    WdfControlFinishInitializing(Device);

    /* Timer, parented to the device */
    KeInitializeEvent(&TimerEvent, NotificationEvent, FALSE);
    WDF_TIMER_CONFIG_INIT(&TimerConfig, Kmdf133EvtTimer);
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfTimerCreate(&TimerConfig, &Attributes, &Timer);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(10));
        Timeout.QuadPart = -10LL * 1000 * 1000 * 5;
        Status = KeWaitForSingleObject(&TimerEvent, Executive, KernelMode, FALSE, &Timeout);
        ok_eq_hex(Status, STATUS_SUCCESS);
        WdfTimerStop(Timer, TRUE);
    }

    /* Work item, parented to the device */
    WDF_WORKITEM_CONFIG_INIT(&WorkItemConfig, Kmdf133EvtWorkItem);
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfWorkItemCreate(&WorkItemConfig, &Attributes, &WorkItem);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        WdfWorkItemEnqueue(WorkItem);
        WdfWorkItemFlush(WorkItem);
        ok_eq_long(WorkItemRuns, 1L);
    }

    WdfObjectDelete(Device);
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
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);
    UNREFERENCED_PARAMETER(OutLength);

    if (ControlCode != IOCTL_KMDF133_RUN_TESTS)
        return STATUS_NOT_SUPPORTED;

    ok_irql(PASSIVE_LEVEL);
    TestStub();
    TestClassExtension();
    TestVersion();
    TestObjects();
    TestRegistry();
    TestControlDevice();
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

    *DeviceName = L"Kmdf133";

    /* kmtest owns the dispatch table, KMDF must leave it alone */
    WDF_DRIVER_CONFIG_INIT(&Config, WDF_NO_EVENT_CALLBACK);
    Config.DriverInitFlags |= WdfDriverInitNonPnpDriver | WdfDriverInitNoDispatchOverride;
    Config.EvtDriverUnload = Kmdf133EvtDriverUnload;

    Status = WdfDriverCreate(DriverObject, (PUNICODE_STRING)RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("Kmdf133: WdfDriverCreate failed 0x%lx\n", Status);
        return Status;
    }

    /* kmtest installs its own unload routine after us; chain to the framework's */
    WdfDriverUnloadRoutine = DriverObject->DriverUnload;

    DbgPrint("Kmdf133: bound to KMDF %lu.%lu, KmtCx globals %p\n",
             WdfBindInfo.V1.Version.Major, WdfBindInfo.V1.Version.Minor, KmtCxDriverGlobals);

    return KmtRegisterMessageHandler(IOCTL_KMDF133_RUN_TESTS, NULL, TestMessageHandler);
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    if (WdfDriverUnloadRoutine)
        WdfDriverUnloadRoutine(DriverObject);
}
