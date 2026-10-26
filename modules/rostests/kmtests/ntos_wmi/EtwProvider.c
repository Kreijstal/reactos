/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite ETW provider smoke test
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

#if (NTDDI_VERSION >= NTDDI_WIN8)

/* {6C1A8C3E-5D4B-4F2A-9E77-0B3A1D2C4E5F}, not registered with any session */
static const GUID TestProviderId =
    { 0x6c1a8c3e, 0x5d4b, 0x4f2a, { 0x9e, 0x77, 0x0b, 0x3a, 0x1d, 0x2c, 0x4e, 0x5f } };

static volatile LONG EnableCallbackCount;

static
VOID
NTAPI
TestEnableCallback(
    _In_ LPCGUID SourceId,
    _In_ ULONG ControlCode,
    _In_ UCHAR Level,
    _In_ ULONGLONG MatchAnyKeyword,
    _In_ ULONGLONG MatchAllKeyword,
    _In_opt_ PEVENT_FILTER_DESCRIPTOR FilterData,
    _Inout_opt_ PVOID CallbackContext)
{
    InterlockedIncrement(&EnableCallbackCount);
}

typedef NTSTATUS (NTAPI *PFN_ETW_REGISTER)(LPCGUID, PETWENABLECALLBACK, PVOID, PREGHANDLE);
typedef NTSTATUS (NTAPI *PFN_ETW_UNREGISTER)(REGHANDLE);
typedef NTSTATUS (NTAPI *PFN_ETW_WRITE_TRANSFER)(REGHANDLE, PCEVENT_DESCRIPTOR, LPCGUID, LPCGUID,
                                                 ULONG, PEVENT_DATA_DESCRIPTOR);
typedef NTSTATUS (NTAPI *PFN_ETW_SET_INFORMATION)(REGHANDLE, EVENT_INFO_CLASS, PVOID, ULONG);
typedef BOOLEAN (NTAPI *PFN_ETW_EVENT_ENABLED)(REGHANDLE, PCEVENT_DESCRIPTOR);

static
PVOID
GetKernelRoutine(
    _In_ PCWSTR Name)
{
    UNICODE_STRING RoutineName;
    PVOID Routine;

    RtlInitUnicodeString(&RoutineName, Name);
    Routine = MmGetSystemRoutineAddress(&RoutineName);
    ok(Routine != NULL, "ntoskrnl does not export %ls\n", Name);
    return Routine;
}

/* The same sequence TraceLogging uses, through the kernel's own exports, the
 * way a driver built with the WDK imports them */
static
VOID
TestKernelExports(VOID)
{
    PFN_ETW_REGISTER pEtwRegister = GetKernelRoutine(L"EtwRegister");
    PFN_ETW_UNREGISTER pEtwUnregister = GetKernelRoutine(L"EtwUnregister");
    PFN_ETW_WRITE_TRANSFER pEtwWriteTransfer = GetKernelRoutine(L"EtwWriteTransfer");
    PFN_ETW_SET_INFORMATION pEtwSetInformation = GetKernelRoutine(L"EtwSetInformation");
    PFN_ETW_EVENT_ENABLED pEtwEventEnabled = GetKernelRoutine(L"EtwEventEnabled");
    UCHAR Traits[] = { 0x05, 0x00, 'K', 'm', 0x00 };
    EVENT_DESCRIPTOR Descriptor;
    EVENT_DATA_DESCRIPTOR Data;
    ULONG Value = 42;
    REGHANDLE Handle = 0;
    NTSTATUS Status;

    if (skip(pEtwRegister && pEtwUnregister && pEtwWriteTransfer && pEtwSetInformation && pEtwEventEnabled,
             "Missing exports\n"))
    {
        return;
    }

    Status = pEtwRegister(&TestProviderId, TestEnableCallback, NULL, &Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Handle != 0, "EtwRegister returned a zero handle\n");
    if (!NT_SUCCESS(Status))
        return;

    Status = pEtwSetInformation(Handle, EventProviderSetTraits, Traits, sizeof(Traits));
    ok_eq_hex(Status, STATUS_SUCCESS);

    EventDescCreate(&Descriptor, 2, 0, 0, 2 /* TRACE_LEVEL_ERROR */, 0, 0, 0);
    EventDataDescCreate(&Data, &Value, sizeof(Value));
    ok_eq_uint(pEtwEventEnabled(Handle, &Descriptor), FALSE);
    Status = pEtwWriteTransfer(Handle, &Descriptor, NULL, NULL, 1, &Data);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = pEtwUnregister(Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

START_TEST(EtwProvider)
{
    REGHANDLE Handle = 0, Handle2 = 0;
    EVENT_DESCRIPTOR Descriptor;
    EVENT_DATA_DESCRIPTOR Data[2];
    ULONG Value = 0x12345678;
    /* TraceLogging provider traits: a UINT16 size, then the provider name */
    UCHAR Traits[] = { 0x0b, 0x00, 'K', 'm', 't', 'E', 't', 'w', 'T', 'e', 0x00 };
    KIRQL OldIrql;
    NTSTATUS Status;

    Status = EtwRegister(&TestProviderId, TestEnableCallback, NULL, &Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Handle != 0, "EtwRegister returned a zero handle\n");
    if (skip(NT_SUCCESS(Status) && Handle != 0, "EtwRegister failed\n"))
        return;

    /* Two registrations of the same provider get different handles */
    Status = EtwRegister(&TestProviderId, NULL, NULL, &Handle2);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Handle2 != 0 && Handle2 != Handle, "Second handle is %I64x, first %I64x\n", Handle2, Handle);

    /* TraceLogging does this right after registering */
    Status = EtwSetInformation(Handle, EventProviderSetTraits, Traits, sizeof(Traits));
    ok_eq_hex(Status, STATUS_SUCCESS);

    EventDescCreate(&Descriptor, 1, 0, 0, 4 /* TRACE_LEVEL_INFORMATION */, 0, 0, 0);
    EventDataDescCreate(&Data[0], &Value, sizeof(Value));
    EventDataDescCreate(&Data[1], L"Kmt", sizeof(L"Kmt"));

    /* Nobody enabled the provider */
    ok_eq_uint(EtwEventEnabled(Handle, &Descriptor), FALSE);
    ok_eq_uint(EtwProviderEnabled(Handle, 4, 0), FALSE);

    /* Writes to a registered provider succeed, with or without data */
    Status = EtwWrite(Handle, &Descriptor, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = EtwWrite(Handle, &Descriptor, NULL, RTL_NUMBER_OF(Data), Data);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = EtwWriteTransfer(Handle, &Descriptor, NULL, NULL, RTL_NUMBER_OF(Data), Data);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = EtwWriteEx(Handle, &Descriptor, 0, 0, NULL, NULL, RTL_NUMBER_OF(Data), Data);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = EtwWriteString(Handle, 4, 0, NULL, L"kmtest");
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Writes are allowed up to HIGH_LEVEL */
    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    Status = EtwWriteTransfer(Handle, &Descriptor, NULL, NULL, RTL_NUMBER_OF(Data), Data);
    KeLowerIrql(OldIrql);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* The second handle is independent */
    Status = EtwUnregister(Handle2);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = EtwWriteTransfer(Handle, &Descriptor, NULL, NULL, 0, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = EtwUnregister(Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Nothing enabled the provider, so the callback never ran */
    ok_eq_long(EnableCallbackCount, 0L);

    /* A zero handle is not a registration */
    Status = EtwWrite(0, &Descriptor, NULL, 0, NULL);
    ok(!NT_SUCCESS(Status), "EtwWrite(0) returned 0x%lx\n", Status);
    Status = EtwWriteTransfer(0, &Descriptor, NULL, NULL, 0, NULL);
    ok(!NT_SUCCESS(Status), "EtwWriteTransfer(0) returned 0x%lx\n", Status);
    Status = EtwSetInformation(0, EventProviderSetTraits, Traits, sizeof(Traits));
    ok(!NT_SUCCESS(Status), "EtwSetInformation(0) returned 0x%lx\n", Status);
    Status = EtwUnregister(0);
    ok(!NT_SUCCESS(Status), "EtwUnregister(0) returned 0x%lx\n", Status);

    TestKernelExports();
    ok_eq_long(EnableCallbackCount, 0L);
}

#else

START_TEST(EtwProvider)
{
    skip(FALSE, "EtwProvider requires NT 6.2 or later\n");
}

#endif
