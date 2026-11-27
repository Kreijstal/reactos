/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite for PoRegisterPowerSettingCallback
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include <kmt_test.h>
#include <acpiioct.h>

/* Resolved at run time like the drivers that use them (usbhub3), so the test
 * calls the ntoskrnl export on every NTDDI configuration */
typedef NTSTATUS
(NTAPI *PFN_SETTING_CALLBACK)(
    _In_ LPCGUID SettingGuid,
    _In_ PVOID Value,
    _In_ ULONG ValueLength,
    _Inout_opt_ PVOID Context);

typedef NTSTATUS
(NTAPI *PFN_REGISTER_SETTING)(
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_ LPCGUID SettingGuid,
    _In_ PFN_SETTING_CALLBACK Callback,
    _In_opt_ PVOID Context,
    _Outptr_opt_ PVOID *Handle);

typedef NTSTATUS
(NTAPI *PFN_UNREGISTER_SETTING)(
    _Inout_ PVOID Handle);

static PFN_REGISTER_SETTING pPoRegisterPowerSettingCallback;
static PFN_UNREGISTER_SETTING pPoUnregisterPowerSettingCallback;

static const GUID TestGuidAcDcSource =
    { 0x5d3e9a59, 0xe9d5, 0x4b00, { 0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48 } };
static const GUID TestGuidConsoleDisplay =
    { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };
static const GUID TestGuidMonitorPowerOn =
    { 0x02731015, 0x4510, 0x4526, { 0x99, 0xe6, 0xe5, 0xa1, 0x7e, 0xbd, 0x1a, 0xea } };
static const GUID TestGuidAwayMode =
    { 0x98a7f580, 0x01f7, 0x48aa, { 0x9c, 0x0f, 0x44, 0x35, 0x2c, 0x29, 0xe5, 0xc0 } };
/* Session-specific, only user-mode recipients get them */
static const GUID TestGuidSessionDisplay =
    { 0x2b84c20e, 0xad23, 0x4ddf, { 0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5 } };
static const GUID TestGuidSessionUserPresence =
    { 0x3c0f4548, 0xc03f, 0x4c4d, { 0xb9, 0xf2, 0x23, 0x7e, 0xde, 0x68, 0x63, 0x76 } };
/* USB settings subgroup: selective suspend and the hub selective suspend timeout */
static const GUID TestGuidUsbSelectiveSuspend =
    { 0x48e6b7a6, 0x50f5, 0x4782, { 0xa5, 0xd4, 0x53, 0xbb, 0x8f, 0x07, 0xe2, 0x26 } };
static const GUID TestGuidUsbHubTimeout =
    { 0x0853a681, 0x27c8, 0x4100, { 0xa2, 0xfd, 0x82, 0x01, 0x3e, 0x97, 0x06, 0x83 } };
static const GUID TestGuidLidSwitch =
    { 0xba3e0f4d, 0xb817, 0x4094, { 0xa2, 0xd1, 0xd5, 0x63, 0x79, 0xe6, 0xa0, 0xf3 } };
/* GUID_DEVICE_LID, the interface of a lid device (poclass.h) */
static const GUID TestGuidDeviceLid =
    { 0x4afa3d52, 0x74a7, 0x11d0, { 0xbe, 0x5e, 0x00, 0xa0, 0xc9, 0x06, 0x28, 0x57 } };
/* No power setting has this GUID */
static const GUID TestGuidUnknown =
    { 0x6b1c4f3e, 0x09d2, 0x4c57, { 0x8a, 0x61, 0x2e, 0x4d, 0x73, 0x90, 0xb5, 0x1f } };

#define USB_SUBGROUP_KEY    L"2a737441-1930-4402-8d77-b2bebba308a3"
#define USB_SUSPEND_KEY     L"48e6b7a6-50f5-4782-a5d4-53bb8f07e226"
#define POWER_KEY           L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Power"

typedef struct _SETTING_CONTEXT
{
    KEVENT Called;
    KEVENT Entered;
    KEVENT Release;
    volatile LONG Count;
    volatile LONG InCallback;
    GUID Guid;
    ULONG Length;
    UCHAR Value[32];
    KIRQL Irql;
    BOOLEAN WaitForRelease;
    BOOLEAN UnregisterSelf;
    PVOID Handle;
    NTSTATUS SelfUnregisterStatus;
} SETTING_CONTEXT, *PSETTING_CONTEXT;

static
NTSTATUS
NTAPI
SettingCallback(
    _In_ LPCGUID SettingGuid,
    _In_ PVOID Value,
    _In_ ULONG ValueLength,
    _Inout_opt_ PVOID Context)
{
    PSETTING_CONTEXT Ctx = Context;

    InterlockedIncrement(&Ctx->InCallback);
    Ctx->Irql = KeGetCurrentIrql();
    Ctx->Guid = *SettingGuid;
    Ctx->Length = ValueLength;
    RtlZeroMemory(Ctx->Value, sizeof(Ctx->Value));
    if (Value != NULL)
        RtlCopyMemory(Ctx->Value, Value, min(ValueLength, sizeof(Ctx->Value)));

    if (Ctx->WaitForRelease)
    {
        KeSetEvent(&Ctx->Entered, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(&Ctx->Release, Executive, KernelMode, FALSE, NULL);
    }

    if (Ctx->UnregisterSelf)
        Ctx->SelfUnregisterStatus = pPoUnregisterPowerSettingCallback(Ctx->Handle);

    InterlockedDecrement(&Ctx->InCallback);
    InterlockedIncrement(&Ctx->Count);
    KeSetEvent(&Ctx->Called, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

static
VOID
InitContext(
    _Out_ PSETTING_CONTEXT Ctx)
{
    RtlZeroMemory(Ctx, sizeof(*Ctx));
    KeInitializeEvent(&Ctx->Called, SynchronizationEvent, FALSE);
    KeInitializeEvent(&Ctx->Entered, NotificationEvent, FALSE);
    KeInitializeEvent(&Ctx->Release, NotificationEvent, FALSE);
    Ctx->Irql = 0xFF;
}

static
NTSTATUS
WaitCalled(
    _In_ PSETTING_CONTEXT Ctx,
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = -10000LL * Milliseconds;
    return KeWaitForSingleObject(&Ctx->Called, Executive, KernelMode, FALSE, &Timeout);
}

/* Registers, checks the initial call and returns the handle */
static
PVOID
RegisterAndCheckInitial(
    _In_ LPCGUID Guid,
    _In_ PCSTR Name,
    _Inout_ PSETTING_CONTEXT Ctx,
    _In_ ULONG ExpectedValue,
    _In_ BOOLEAN AnyValue)
{
    NTSTATUS Status;
    PVOID Handle = (PVOID)(ULONG_PTR)0x55555555;

    Status = pPoRegisterPowerSettingCallback(NULL, Guid, SettingCallback, Ctx, &Handle);
    ok(Status == STATUS_SUCCESS, "%s: register returned 0x%08lx\n", Name, Status);
    if (!NT_SUCCESS(Status))
        return NULL;
    ok(Handle != NULL && Handle != (PVOID)(ULONG_PTR)0x55555555, "%s: handle %p\n", Name, Handle);

    /* The power manager calls the callback once with the current value */
    Status = WaitCalled(Ctx, 10000);
    ok(Status == STATUS_SUCCESS, "%s: no initial callback (0x%08lx)\n", Name, Status);
    if (Status == STATUS_SUCCESS)
    {
        ok(Ctx->Irql == PASSIVE_LEVEL, "%s: callback at IRQL %u\n", Name, Ctx->Irql);
        ok(IsEqualGUID(&Ctx->Guid, Guid), "%s: callback for another GUID\n", Name);
        ok(Ctx->Length == sizeof(ULONG), "%s: value length %lu\n", Name, Ctx->Length);
        if (AnyValue)
            ok(*(PULONG)Ctx->Value <= ExpectedValue, "%s: value %lu, max %lu\n", Name, *(PULONG)Ctx->Value, ExpectedValue);
        else
            ok(*(PULONG)Ctx->Value == ExpectedValue, "%s: value %lu, expected %lu\n", Name, *(PULONG)Ctx->Value, ExpectedValue);
    }

    /* and only once while the value does not change */
    Status = WaitCalled(Ctx, 500);
    ok(Status == STATUS_TIMEOUT, "%s: second callback without a change (0x%08lx)\n", Name, Status);
    ok(Ctx->Count == 1, "%s: %ld callbacks\n", Name, Ctx->Count);
    return Handle;
}

static
VOID
TestInitialValues(VOID)
{
    static SETTING_CONTEXT Ctx[6];
    static const struct
    {
        LPCGUID Guid;
        PCSTR Name;
        ULONG Value;
        BOOLEAN Any;
    } Tests[] =
    {
        /* PoAc, PoDc or PoHot; QEMU and machines without a battery are on AC */
        { &TestGuidAcDcSource,          "ACDC_POWER_SOURCE",    2,  TRUE  },
        { &TestGuidConsoleDisplay,      "CONSOLE_DISPLAY_STATE",1,  FALSE },
        { &TestGuidMonitorPowerOn,      "MONITOR_POWER_ON",     1,  FALSE },
        { &TestGuidAwayMode,            "SYSTEM_AWAYMODE",      0,  FALSE },
        /* Power scheme values (Balanced, both sources) */
        { &TestGuidUsbSelectiveSuspend, "USB selective suspend",1,  FALSE },
        { &TestGuidUsbHubTimeout,       "USB hub timeout",      50, FALSE },
    };
    PVOID Handle[RTL_NUMBER_OF(Tests)];
    ULONG i;
    NTSTATUS Status;

    for (i = 0; i < RTL_NUMBER_OF(Tests); i++)
    {
        InitContext(&Ctx[i]);
        Handle[i] = RegisterAndCheckInitial(Tests[i].Guid, Tests[i].Name, &Ctx[i], Tests[i].Value, Tests[i].Any);
    }

    for (i = 0; i < RTL_NUMBER_OF(Tests); i++)
    {
        if (Handle[i] == NULL)
            continue;
        Status = pPoUnregisterPowerSettingCallback(Handle[i]);
        ok(Status == STATUS_SUCCESS, "%s: unregister returned 0x%08lx\n", Tests[i].Name, Status);
    }
}

static
VOID
TestTwoRegistrations(VOID)
{
    static SETTING_CONTEXT Ctx1, Ctx2;
    PVOID Handle1, Handle2;
    NTSTATUS Status;

    /* Every registration gets its own initial call */
    InitContext(&Ctx1);
    InitContext(&Ctx2);
    Handle1 = RegisterAndCheckInitial(&TestGuidAcDcSource, "ACDC #1", &Ctx1, 2, TRUE);
    Handle2 = RegisterAndCheckInitial(&TestGuidAcDcSource, "ACDC #2", &Ctx2, 2, TRUE);
    ok(Handle1 != Handle2, "Same handle %p for two registrations\n", Handle1);
    ok(Ctx1.Count == 1, "First registration called %ld times\n", Ctx1.Count);
    if (Handle1)
    {
        Status = pPoUnregisterPowerSettingCallback(Handle1);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    if (Handle2)
    {
        Status = pPoUnregisterPowerSettingCallback(Handle2);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
}

static
VOID
TestUnknownAndInvalid(VOID)
{
    static SETTING_CONTEXT Ctx;
    PVOID Handle;
    NTSTATUS Status;
    ULONG Fake[32];

    /* A GUID nobody defines registers, but there is no value to report */
    InitContext(&Ctx);
    Handle = NULL;
    Status = pPoRegisterPowerSettingCallback(NULL, &TestGuidUnknown, SettingCallback, &Ctx, &Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Handle != NULL, "No handle for an unknown setting\n");
    Status = WaitCalled(&Ctx, 2000);
    ok_eq_hex(Status, STATUS_TIMEOUT);
    ok_eq_long(Ctx.Count, 0L);
    if (Handle)
    {
        Status = pPoUnregisterPowerSettingCallback(Handle);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    /* Session-specific settings are not for drivers */
    InitContext(&Ctx);
    Handle = (PVOID)(ULONG_PTR)0x55555555;
    Status = pPoRegisterPowerSettingCallback(NULL, &TestGuidSessionDisplay, SettingCallback, &Ctx, &Handle);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok_eq_pointer(Handle, (PVOID)(ULONG_PTR)0x55555555);
    Status = pPoRegisterPowerSettingCallback(NULL, &TestGuidSessionUserPresence, SettingCallback, &Ctx, &Handle);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok_eq_pointer(Handle, (PVOID)(ULONG_PTR)0x55555555);
    Status = WaitCalled(&Ctx, 500);
    ok_eq_hex(Status, STATUS_TIMEOUT);

    /* Handles that are not registrations */
    Status = pPoUnregisterPowerSettingCallback(NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    RtlZeroMemory(Fake, sizeof(Fake));
    Status = pPoUnregisterPowerSettingCallback(Fake);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
}

/* Evaluates _LID of the lid device behind SymbolicLink (IOCTL_ACPI_EVAL_METHOD) */
static
NTSTATUS
EvaluateLid(
    _In_ PUNICODE_STRING SymbolicLink,
    _Out_ PULONG LidState)
{
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT DeviceObject, Pdo;
    ACPI_EVAL_INPUT_BUFFER Input;
    UCHAR OutputBuffer[sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 16];
    PACPI_EVAL_OUTPUT_BUFFER Output = (PACPI_EVAL_OUTPUT_BUFFER)OutputBuffer;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    Status = IoGetDeviceObjectPointer(SymbolicLink, FILE_READ_DATA, &FileObject, &DeviceObject);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The ACPI PDO evaluates the methods of its namespace object */
    Pdo = IoGetDeviceAttachmentBaseRef(DeviceObject);
    ObDereferenceObject(FileObject);

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(Input.MethodName, "_LID", 4);
    RtlZeroMemory(OutputBuffer, sizeof(OutputBuffer));

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IOCTL_ACPI_EVAL_METHOD, Pdo,
                                        &Input, sizeof(Input),
                                        OutputBuffer, sizeof(OutputBuffer),
                                        FALSE, &Event, &IoStatus);
    if (!Irp)
    {
        ObDereferenceObject(Pdo);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = IoCallDriver(Pdo, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }
    ObDereferenceObject(Pdo);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Output->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || Output->Count < 1 ||
        Output->Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER)
    {
        return STATUS_UNSUCCESSFUL;
    }

    *LidState = (Output->Argument[0].Argument != 0) ? 1 : 0;
    return STATUS_SUCCESS;
}

/*
 * GUID_LIDSWITCH_STATE_CHANGE: "The callback won't be called at all until a
 * lid device is found and its current state is known" (0 closed, 1 open).
 * With a lid, a registration made after boot gets the state at once.
 */
static
VOID
TestLidSwitch(VOID)
{
    static SETTING_CONTEXT Ctx;
    PWSTR SymbolicLinks = NULL;
    UNICODE_STRING Link;
    ULONG LidState = 0;
    PVOID Handle;
    NTSTATUS Status;

    Status = IoGetDeviceInterfaces((LPGUID)&TestGuidDeviceLid, NULL, 0, &SymbolicLinks);
    if (!NT_SUCCESS(Status) || SymbolicLinks == NULL || SymbolicLinks[0] == UNICODE_NULL)
    {
        /* No lid: the setting never gets a value */
        trace("No lid device (0x%08lx)\n", Status);
        if (SymbolicLinks)
            ExFreePool(SymbolicLinks);

        InitContext(&Ctx);
        Handle = NULL;
        Status = pPoRegisterPowerSettingCallback(NULL, &TestGuidLidSwitch, SettingCallback, &Ctx, &Handle);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = WaitCalled(&Ctx, 2000);
        ok_eq_hex(Status, STATUS_TIMEOUT);
        ok_eq_long(Ctx.Count, 0L);
        if (Handle)
        {
            Status = pPoUnregisterPowerSettingCallback(Handle);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
        return;
    }

    RtlInitUnicodeString(&Link, SymbolicLinks);
    Status = EvaluateLid(&Link, &LidState);
    ok(NT_SUCCESS(Status), "Evaluating _LID of %wZ failed: 0x%08lx\n", &Link, Status);
    ExFreePool(SymbolicLinks);
    if (!NT_SUCCESS(Status))
        return;
    trace("Lid is %s\n", LidState ? "open" : "closed");

    InitContext(&Ctx);
    Handle = RegisterAndCheckInitial(&TestGuidLidSwitch, "LIDSWITCH_STATE_CHANGE", &Ctx, LidState, FALSE);
    if (Handle)
    {
        Status = pPoUnregisterPowerSettingCallback(Handle);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
}

typedef struct _UNREGISTER_WORK
{
    WORK_QUEUE_ITEM Item;
    PVOID Handle;
    NTSTATUS Status;
    LONG InCallbackAfter;
    PSETTING_CONTEXT Ctx;
    KEVENT Done;
} UNREGISTER_WORK, *PUNREGISTER_WORK;

static
VOID
NTAPI
UnregisterWorker(
    _In_ PVOID Parameter)
{
    PUNREGISTER_WORK Work = Parameter;

    Work->Status = pPoUnregisterPowerSettingCallback(Work->Handle);
    Work->InCallbackAfter = Work->Ctx->InCallback;
    KeSetEvent(&Work->Done, IO_NO_INCREMENT, FALSE);
}

static
VOID
TestUnregisterFromOtherThread(VOID)
{
    static SETTING_CONTEXT Ctx;
    static UNREGISTER_WORK Work;
    PVOID Handle = NULL;
    NTSTATUS Status;
    LARGE_INTEGER Timeout;

    InitContext(&Ctx);
    Ctx.WaitForRelease = TRUE;
    Status = pPoRegisterPowerSettingCallback(NULL, &TestGuidAcDcSource, SettingCallback, &Ctx, &Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Timeout.QuadPart = -10000LL * 10000;
    Status = KeWaitForSingleObject(&Ctx.Entered, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Unregister on a worker while the callback is blocked: it must not return */
    RtlZeroMemory(&Work, sizeof(Work));
    Work.Handle = Handle;
    Work.Ctx = &Ctx;
    Work.Status = 0x12345678;
    KeInitializeEvent(&Work.Done, NotificationEvent, FALSE);
    ExInitializeWorkItem(&Work.Item, UnregisterWorker, &Work);
    ExQueueWorkItem(&Work.Item, DelayedWorkQueue);

    Timeout.QuadPart = -10000LL * 1000;
    Status = KeWaitForSingleObject(&Work.Done, Executive, KernelMode, FALSE, &Timeout);
    ok(Status == STATUS_TIMEOUT, "Unregister returned while the callback runs (0x%08lx)\n", Status);

    KeSetEvent(&Ctx.Release, IO_NO_INCREMENT, FALSE);
    Timeout.QuadPart = -10000LL * 10000;
    Status = KeWaitForSingleObject(&Work.Done, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_hex(Work.Status, STATUS_SUCCESS);
    ok_eq_long(Work.InCallbackAfter, 0L);
    if (Status != STATUS_SUCCESS)
    {
        /* Do not unload with the work item outstanding */
        KeWaitForSingleObject(&Work.Done, Executive, KernelMode, FALSE, NULL);
    }
}

static
VOID
TestUnregisterFromCallback(VOID)
{
    static SETTING_CONTEXT Ctx;
    PVOID Handle = NULL;
    NTSTATUS Status;

    /* A callback may unregister itself */
    InitContext(&Ctx);
    Ctx.UnregisterSelf = TRUE;
    Ctx.SelfUnregisterStatus = 0x12345678;
    Status = pPoRegisterPowerSettingCallback(NULL, &TestGuidAcDcSource, SettingCallback, &Ctx, &Ctx.Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    Handle = Ctx.Handle;
    Status = WaitCalled(&Ctx, 10000);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (Status != STATUS_SUCCESS)
    {
        pPoUnregisterPowerSettingCallback(Handle);
        return;
    }
    ok_eq_hex(Ctx.SelfUnregisterStatus, STATUS_SUCCESS);
}

static
NTSTATUS
OpenOrCreateKey(
    _Out_ PHANDLE KeyHandle,
    _In_opt_ HANDLE Root,
    _In_ PCWSTR Name,
    _In_ BOOLEAN Create,
    _Out_opt_ PULONG Disposition)
{
    UNICODE_STRING KeyName;
    OBJECT_ATTRIBUTES ObjectAttributes;

    RtlInitUnicodeString(&KeyName, Name);
    InitializeObjectAttributes(&ObjectAttributes, &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Root, NULL);
    if (!Create)
        return ZwOpenKey(KeyHandle, KEY_ALL_ACCESS, &ObjectAttributes);
    return ZwCreateKey(KeyHandle, KEY_ALL_ACCESS, &ObjectAttributes, 0, NULL,
                       REG_OPTION_VOLATILE, Disposition);
}

static
NTSTATUS
SetDword(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _In_ ULONG Value)
{
    UNICODE_STRING ValueName;

    RtlInitUnicodeString(&ValueName, Name);
    return ZwSetValueKey(Key, &ValueName, 0, REG_DWORD, &Value, sizeof(Value));
}

/* Overrides the active scheme's USB selective suspend value the way the power
 * scheme store holds it, and expects the registered callback to follow */
static
VOID
TestSchemeChange(VOID)
{
    static SETTING_CONTEXT Ctx;
    PVOID Handle;
    NTSTATUS Status;
    HANDLE PowerKey = NULL, SchemesKey = NULL, SchemeKey = NULL, SubKey = NULL, SettingKey = NULL;
    ULONG SubDisp = 0, SettingDisp = 0;
    UNICODE_STRING ValueName;
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 80 * sizeof(WCHAR)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    WCHAR Scheme[40];
    ULONG Length;

    InitContext(&Ctx);
    Handle = RegisterAndCheckInitial(&TestGuidUsbSelectiveSuspend, "USB suspend", &Ctx, 1, FALSE);
    if (Handle == NULL)
        return;

    Status = OpenOrCreateKey(&PowerKey, NULL, POWER_KEY, FALSE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Status = OpenOrCreateKey(&SchemesKey, PowerKey, L"User\\PowerSchemes", FALSE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    RtlInitUnicodeString(&ValueName, L"ActivePowerScheme");
    RtlZeroMemory(Buffer, sizeof(Buffer));
    Status = ZwQueryValueKey(SchemesKey, &ValueName, KeyValuePartialInformation,
                             Info, sizeof(Buffer) - sizeof(WCHAR), &Length);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    ok_eq_ulong(Info->Type, (ULONG)REG_SZ);
    RtlZeroMemory(Scheme, sizeof(Scheme));
    RtlCopyMemory(Scheme, Info->Data, min(Info->DataLength, sizeof(Scheme) - sizeof(WCHAR)));

    Status = OpenOrCreateKey(&SchemeKey, SchemesKey, Scheme, FALSE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Status = OpenOrCreateKey(&SubKey, SchemeKey, USB_SUBGROUP_KEY, TRUE, &SubDisp);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    Status = OpenOrCreateKey(&SettingKey, SubKey, USB_SUSPEND_KEY, TRUE, &SettingDisp);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    ok_eq_ulong(SettingDisp, (ULONG)REG_CREATED_NEW_KEY);

    /* Disable selective suspend on both power sources */
    Status = SetDword(SettingKey, L"DCSettingIndex", 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = SetDword(SettingKey, L"ACSettingIndex", 0);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = WaitCalled(&Ctx, 10000);
    ok(Status == STATUS_SUCCESS, "No callback after the scheme value changed (0x%08lx)\n", Status);
    if (Status == STATUS_SUCCESS)
    {
        /* Both values may land in one or two notifications */
        if (*(PULONG)Ctx.Value != 0)
            WaitCalled(&Ctx, 10000);
        ok_eq_ulong(Ctx.Length, (ULONG)sizeof(ULONG));
        ok_eq_ulong(*(PULONG)Ctx.Value, 0UL);
        ok(Ctx.Irql == PASSIVE_LEVEL, "Callback at IRQL %u\n", Ctx.Irql);
    }

    /* Removing the override returns to the scheme default */
    if (SettingDisp == REG_CREATED_NEW_KEY)
    {
        Status = ZwDeleteKey(SettingKey);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = WaitCalled(&Ctx, 10000);
        ok(Status == STATUS_SUCCESS, "No callback after the override was removed (0x%08lx)\n", Status);
        ok_eq_ulong(*(PULONG)Ctx.Value, 1UL);
    }

Cleanup:
    if (SettingKey)
        ZwClose(SettingKey);
    if (SubKey)
    {
        if (SubDisp == REG_CREATED_NEW_KEY)
            ZwDeleteKey(SubKey);
        ZwClose(SubKey);
    }
    if (SchemeKey)
        ZwClose(SchemeKey);
    if (SchemesKey)
        ZwClose(SchemesKey);
    if (PowerKey)
        ZwClose(PowerKey);

    Status = pPoUnregisterPowerSettingCallback(Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

START_TEST(PoPowerSettingCallback)
{
    UNICODE_STRING Name;

    RtlInitUnicodeString(&Name, L"PoRegisterPowerSettingCallback");
    pPoRegisterPowerSettingCallback = (PFN_REGISTER_SETTING)MmGetSystemRoutineAddress(&Name);
    RtlInitUnicodeString(&Name, L"PoUnregisterPowerSettingCallback");
    pPoUnregisterPowerSettingCallback = (PFN_UNREGISTER_SETTING)MmGetSystemRoutineAddress(&Name);
    if (!pPoRegisterPowerSettingCallback || !pPoUnregisterPowerSettingCallback)
    {
        skip(FALSE, "PoRegisterPowerSettingCallback is not exported\n");
        return;
    }

    TestInitialValues();
    TestLidSwitch();
    TestTwoRegistrations();
    TestUnknownAndInvalid();
    TestUnregisterFromOtherThread();
    TestUnregisterFromCallback();
    TestSchemeChange();
}
