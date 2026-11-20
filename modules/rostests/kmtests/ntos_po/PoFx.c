/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite for the power framework (PoFx)
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * A device with two components goes through the power framework's
 * handshakes: idle and active conditions, F-state changes of the component
 * that has a deeper F-state, device power not required once all components
 * are idle and the idle timeout elapsed, device power required before a
 * component becomes active again, the debounce of a component idled again
 * right away, blocking and asynchronous activations, delayed completions,
 * and unregistering a device whose power is not required. Without a PEP
 * (ReactOS, a classic ACPI PC) the framework itself makes these decisions.
 */

#include <kmt_test.h>

#if (NTDDI_VERSION >= NTDDI_WIN8)

/* Resolved at run time, so the test calls the ntoskrnl exports in every NTDDI configuration */
typedef NTSTATUS (NTAPI *PFN_REGISTER)(PDEVICE_OBJECT, PPO_FX_DEVICE, POHANDLE *);
typedef VOID (NTAPI *PFN_HANDLE)(POHANDLE);
typedef VOID (NTAPI *PFN_COMPONENT)(POHANDLE, ULONG);
typedef VOID (NTAPI *PFN_COMPONENT_FLAGS)(POHANDLE, ULONG, ULONG);
typedef VOID (NTAPI *PFN_TIMEOUT)(POHANDLE, ULONGLONG);
typedef VOID (NTAPI *PFN_COMPONENT_VALUE)(POHANDLE, ULONG, ULONGLONG);
typedef NTSTATUS (NTAPI *PFN_POWER_CONTROL)(POHANDLE, LPCGUID, PVOID, SIZE_T, PVOID, SIZE_T, PSIZE_T);

static PFN_REGISTER pPoFxRegisterDevice;
static PFN_HANDLE pPoFxStartDevicePowerManagement;
static PFN_HANDLE pPoFxUnregisterDevice;
static PFN_COMPONENT_FLAGS pPoFxActivateComponent;
static PFN_COMPONENT_FLAGS pPoFxIdleComponent;
static PFN_COMPONENT pPoFxCompleteIdleCondition;
static PFN_COMPONENT pPoFxCompleteIdleState;
static PFN_HANDLE pPoFxCompleteDevicePowerNotRequired;
static PFN_HANDLE pPoFxReportDevicePoweredOn;
static PFN_TIMEOUT pPoFxSetDeviceIdleTimeout;
static PFN_COMPONENT_VALUE pPoFxSetComponentLatency;
static PFN_POWER_CONTROL pPoFxPowerControl;

#ifndef PO_FX_FLAG_BLOCKING
#define PO_FX_FLAG_BLOCKING 0x00000001
#define PO_FX_FLAG_ASYNC_ONLY 0x00000002
#endif

typedef enum _FX_EVENT_TYPE
{
    FxActive = 1,
    FxIdle,
    FxIdleState,
    FxPowerRequired,
    FxPowerNotRequired,
} FX_EVENT_TYPE;

typedef struct _FX_EVENT
{
    FX_EVENT_TYPE Type;
    ULONG Component;
    ULONG State;
    KIRQL Irql;
    PKTHREAD Thread;
} FX_EVENT;

#define MAX_EVENTS 64

typedef struct _FX_TEST
{
    POHANDLE Handle;
    KSPIN_LOCK Lock;
    ULONG Count;
    FX_EVENT Events[MAX_EVENTS];
    /* Complete inside the callback, or leave it to the test */
    BOOLEAN CompleteIdle;
    BOOLEAN CompleteIdleState;
    BOOLEAN CompleteNotRequired;
    /* Report the device powered on from inside the power required callback */
    BOOLEAN PowerOnInCallback;
} FX_TEST, *PFX_TEST;

static FX_TEST Test;

/* No PEP knows this power control code */
static const GUID TestPowerControlCode =
    { 0x2c6f2b9a, 0x5d1e, 0x4e57, { 0x9b, 0x0c, 0x6a, 0x41, 0x73, 0x8e, 0x22, 0xd5 } };

static
VOID
LogEvent(
    _In_ FX_EVENT_TYPE Type,
    _In_ ULONG Component,
    _In_ ULONG State)
{
    KIRQL OldIrql, Irql = KeGetCurrentIrql();

    KeAcquireSpinLock(&Test.Lock, &OldIrql);
    if (Test.Count < MAX_EVENTS)
    {
        Test.Events[Test.Count].Type = Type;
        Test.Events[Test.Count].Component = Component;
        Test.Events[Test.Count].State = State;
        Test.Events[Test.Count].Irql = Irql;
        Test.Events[Test.Count].Thread = KeGetCurrentThread();
    }
    Test.Count++;
    KeReleaseSpinLock(&Test.Lock, OldIrql);
}

static
VOID
NTAPI
ActiveCallback(
    _In_ PVOID Context,
    _In_ ULONG Component)
{
    ok_eq_pointer(Context, &Test);
    LogEvent(FxActive, Component, 0);
}

static
VOID
NTAPI
IdleCallback(
    _In_ PVOID Context,
    _In_ ULONG Component)
{
    ok_eq_pointer(Context, &Test);
    LogEvent(FxIdle, Component, 0);
    if (Test.CompleteIdle)
        pPoFxCompleteIdleCondition(Test.Handle, Component);
}

static
VOID
NTAPI
IdleStateCallback(
    _In_ PVOID Context,
    _In_ ULONG Component,
    _In_ ULONG State)
{
    ok_eq_pointer(Context, &Test);
    LogEvent(FxIdleState, Component, State);
    if (Test.CompleteIdleState)
        pPoFxCompleteIdleState(Test.Handle, Component);
}

static
VOID
NTAPI
PowerRequiredCallback(
    _In_ PVOID Context)
{
    ok_eq_pointer(Context, &Test);
    LogEvent(FxPowerRequired, 0, 0);
    if (Test.PowerOnInCallback)
        pPoFxReportDevicePoweredOn(Test.Handle);
}

static
VOID
NTAPI
PowerNotRequiredCallback(
    _In_ PVOID Context)
{
    ok_eq_pointer(Context, &Test);
    LogEvent(FxPowerNotRequired, 0, 0);
    if (Test.CompleteNotRequired)
        pPoFxCompleteDevicePowerNotRequired(Test.Handle);
}

static
ULONG
EventCount(VOID)
{
    KIRQL OldIrql;
    ULONG Count;

    KeAcquireSpinLock(&Test.Lock, &OldIrql);
    Count = Test.Count;
    KeReleaseSpinLock(&Test.Lock, OldIrql);
    return Count;
}

/* Waits up to TimeoutMs for the log to hold Count events */
static
BOOLEAN
WaitForEvents(
    _In_ ULONG Count,
    _In_ ULONG TimeoutMs)
{
    LARGE_INTEGER Interval;
    ULONG Waited;

    Interval.QuadPart = -10 * 1000 * 10;
    for (Waited = 0; Waited <= TimeoutMs; Waited += 10)
    {
        if (EventCount() >= Count)
            return TRUE;
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    }
    return FALSE;
}

static
VOID
SleepMs(
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -10LL * 1000 * Milliseconds;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

#define ok_event(Index, ExpectedType, ExpectedComponent, ExpectedState) \
    do { \
        ULONG _i = (Index); \
        ok(_i < Test.Count && Test.Events[_i].Type == (ExpectedType) && \
           Test.Events[_i].Component == (ExpectedComponent) && \
           Test.Events[_i].State == (ExpectedState), \
           "Event %lu: type %d component %lu state %lu, expected %d %lu %lu (count %lu)\n", \
           _i, _i < MAX_EVENTS ? Test.Events[_i].Type : 0, \
           _i < MAX_EVENTS ? Test.Events[_i].Component : 0, \
           _i < MAX_EVENTS ? Test.Events[_i].State : 0, \
           (ExpectedType), (ULONG)(ExpectedComponent), (ULONG)(ExpectedState), Test.Count); \
    } while (0)

/* Component 0: F0 only. Component 1: F0 and F1 (1 ms latency, 300 ms residency). */
static PO_FX_COMPONENT_IDLE_STATE F0Only[1] = { { 0, 0, PO_FX_UNKNOWN_POWER } };
static PO_FX_COMPONENT_IDLE_STATE F0F1[2] =
{
    { 0, 0, PO_FX_UNKNOWN_POWER },
    { 10 * 1000, 300 * 10 * 1000, 0 },
};

typedef struct _TEST_DEVICE_V1
{
    PO_FX_DEVICE_V1 Device;
    PO_FX_COMPONENT_V1 Component1;
} TEST_DEVICE_V1;

static
VOID
InitDevice(
    _Out_ TEST_DEVICE_V1 *Info)
{
    RtlZeroMemory(Info, sizeof(*Info));
    Info->Device.Version = PO_FX_VERSION_V1;
    Info->Device.ComponentCount = 2;
    Info->Device.ComponentActiveConditionCallback = ActiveCallback;
    Info->Device.ComponentIdleConditionCallback = IdleCallback;
    Info->Device.ComponentIdleStateCallback = IdleStateCallback;
    Info->Device.DevicePowerRequiredCallback = PowerRequiredCallback;
    Info->Device.DevicePowerNotRequiredCallback = PowerNotRequiredCallback;
    Info->Device.DeviceContext = &Test;
    Info->Device.Components[0].IdleStateCount = 1;
    Info->Device.Components[0].IdleStates = F0Only;
    Info->Component1.IdleStateCount = 2;
    Info->Component1.DeepestWakeableIdleState = 1;
    Info->Component1.IdleStates = F0F1;
}

static
VOID
ResetLog(VOID)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&Test.Lock, &OldIrql);
    Test.Count = 0;
    RtlZeroMemory(Test.Events, sizeof(Test.Events));
    KeReleaseSpinLock(&Test.Lock, OldIrql);
}

static
VOID
TestRegistration(
    _In_ PDEVICE_OBJECT Pdo)
{
    TEST_DEVICE_V1 Info;
    struct
    {
        PO_FX_DEVICE_V3 Device;
        PO_FX_COMPONENT_V2 Component1;
    } V3;
    POHANDLE Handle;
    NTSTATUS Status;

    /* What is refused */
    InitDevice(&Info);
    Info.Device.Version = 0;
    Status = pPoFxRegisterDevice(Pdo, (PPO_FX_DEVICE)&Info.Device, &Handle);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Info.Device.Version = 4;
    Status = pPoFxRegisterDevice(Pdo, (PPO_FX_DEVICE)&Info.Device, &Handle);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    InitDevice(&Info);
    Info.Device.ComponentCount = 0;
    Status = pPoFxRegisterDevice(Pdo, (PPO_FX_DEVICE)&Info.Device, &Handle);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    InitDevice(&Info);
    Info.Component1.IdleStateCount = 0;
    Status = pPoFxRegisterDevice(Pdo, (PPO_FX_DEVICE)&Info.Device, &Handle);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    /* A component with deeper F-states needs the condition and state callbacks */
    InitDevice(&Info);
    Info.Device.ComponentIdleStateCallback = NULL;
    Status = pPoFxRegisterDevice(Pdo, (PPO_FX_DEVICE)&Info.Device, &Handle);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    InitDevice(&Info);
    Info.Component1.DeepestWakeableIdleState = 2;
    Status = pPoFxRegisterDevice(Pdo, (PPO_FX_DEVICE)&Info.Device, &Handle);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Status = pPoFxRegisterDevice(NULL, (PPO_FX_DEVICE)&Info.Device, &Handle);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    /* Directed power transitions come in pairs */
    RtlZeroMemory(&V3, sizeof(V3));
    V3.Device.Version = PO_FX_VERSION_V3;
    V3.Device.ComponentCount = 1;
    V3.Device.Components[0].IdleStateCount = 1;
    V3.Device.Components[0].IdleStates = F0Only;
    V3.Device.DirectedPowerUpCallback = (PPO_FX_DIRECTED_POWER_UP_CALLBACK)ActiveCallback;
    Status = pPoFxRegisterDevice(Pdo, (PPO_FX_DEVICE)&V3.Device, &Handle);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    /* A single F0 component needs no callbacks at all; a V3 device registers */
    V3.Device.DirectedPowerUpCallback = NULL;
    Handle = NULL;
    Status = pPoFxRegisterDevice(Pdo, (PPO_FX_DEVICE)&V3.Device, &Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(Handle != NULL, "No handle\n");
    if (NT_SUCCESS(Status))
    {
        /* Idle at start, power not required, then active again: all without callbacks */
        pPoFxStartDevicePowerManagement(Handle);
        pPoFxActivateComponent(Handle, 0, PO_FX_FLAG_BLOCKING);
        pPoFxIdleComponent(Handle, 0, 0);
        pPoFxUnregisterDevice(Handle);
    }

    /* Unregistering a device that was never started */
    InitDevice(&Info);
    Status = pPoFxRegisterDevice(Pdo, (PPO_FX_DEVICE)&Info.Device, &Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
        pPoFxUnregisterDevice(Handle);
    ok_eq_ulong(EventCount(), 0UL);
}

static
VOID
TestLifecycle(
    _In_ PDEVICE_OBJECT Pdo)
{
    TEST_DEVICE_V1 Info;
    NTSTATUS Status;
    SIZE_T Returned;
    ULONG n, i;
    BOOLEAN Found;

    ResetLog();
    Test.CompleteIdle = TRUE;
    Test.CompleteIdleState = TRUE;
    Test.CompleteNotRequired = TRUE;
    Test.PowerOnInCallback = FALSE;

    InitDevice(&Info);
    Status = pPoFxRegisterDevice(Pdo, (PPO_FX_DEVICE)&Info.Device, &Test.Handle);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* Nothing to ask the PEP: there is none */
    Returned = 123;
    Status = pPoFxPowerControl(Test.Handle, &TestPowerControlCode, NULL, 0, NULL, 0, &Returned);
    ok_eq_hex(Status, STATUS_NOT_SUPPORTED);
    ok_eq_size(Returned, 0);

    /* Registered components are active: no callback, not even for an extra reference */
    pPoFxActivateComponent(Test.Handle, 0, 0);
    pPoFxSetDeviceIdleTimeout(Test.Handle, 1500 * 10 * 1000);
    ok_eq_ulong(EventCount(), 0UL);

    /* Starting drops the registration reference: component 1 goes idle, inline at DISPATCH_LEVEL */
    pPoFxStartDevicePowerManagement(Test.Handle);
    ok_eq_ulong(EventCount(), 1UL);
    ok_event(0, FxIdle, 1, 0);
    ok_eq_uint(Test.Events[0].Irql, DISPATCH_LEVEL);
    ok_eq_pointer(Test.Events[0].Thread, KeGetCurrentThread());

    /* All idle: the idle timeout starts */
    pPoFxIdleComponent(Test.Handle, 0, 0);
    ok_eq_ulong(EventCount(), 2UL);
    ok_event(1, FxIdle, 0, 0);

    /* Idle long enough for F1's residency: component 1 goes to F1 before the timeout */
    ok(WaitForEvents(3, 1200), "No F-state change\n");
    ok_event(2, FxIdleState, 1, 1);

    /* The idle timeout elapsed: device power not required, at DISPATCH_LEVEL */
    ok(WaitForEvents(4, 1500), "No device power not required\n");
    ok_event(3, FxPowerNotRequired, 0, 0);
    ok_eq_uint(Test.Events[3].Irql, DISPATCH_LEVEL);
    SleepMs(100);
    ok_eq_ulong(EventCount(), 4UL);

    /* Activating: device power required first, inline at DISPATCH_LEVEL */
    pPoFxActivateComponent(Test.Handle, 1, 0);
    ok_eq_ulong(EventCount(), 5UL);
    ok_event(4, FxPowerRequired, 0, 0);
    ok_eq_uint(Test.Events[4].Irql, DISPATCH_LEVEL);
    SleepMs(100);
    ok_eq_ulong(EventCount(), 5UL);

    /* Powered: back to F0, then active */
    pPoFxReportDevicePoweredOn(Test.Handle);
    ok(WaitForEvents(7, 1000), "Not active after power-on\n");
    ok_event(5, FxIdleState, 1, 0);
    ok_event(6, FxActive, 1, 0);

    /* From now on component 1 tolerates no latency: it stays in F0 */
    pPoFxSetComponentLatency(Test.Handle, 1, 0);

    /* A blocking activation returns once the component is active */
    pPoFxActivateComponent(Test.Handle, 0, PO_FX_FLAG_BLOCKING);
    ok_eq_ulong(EventCount(), 8UL);
    ok_event(7, FxActive, 0, 0);

    /* Idle component 1 (not debounced: its last idle was long ago) */
    pPoFxIdleComponent(Test.Handle, 1, 0);
    ok_eq_ulong(EventCount(), 9UL);
    ok_event(8, FxIdle, 1, 0);

    /* Active and idle again at once: the idle is debounced for a while */
    pPoFxActivateComponent(Test.Handle, 1, 0);
    ok_eq_ulong(EventCount(), 10UL);
    ok_event(9, FxActive, 1, 0);
    pPoFxIdleComponent(Test.Handle, 1, 0);
    SleepMs(200);
    ok_eq_ulong(EventCount(), 10UL);
    ok(WaitForEvents(11, 2000), "Debounced idle never came\n");
    ok_event(10, FxIdle, 1, 0);
    ok(Test.Events[10].Thread != KeGetCurrentThread(), "Debounced idle on the caller's thread\n");

    /* An idle condition completed late: the activation waits for it */
    Test.CompleteIdle = FALSE;
    SleepMs(600);
    n = EventCount();
    pPoFxIdleComponent(Test.Handle, 0, 0);
    ok_eq_ulong(EventCount(), n + 1);
    ok_event(n, FxIdle, 0, 0);
    pPoFxActivateComponent(Test.Handle, 0, 0);
    SleepMs(100);
    ok_eq_ulong(EventCount(), n + 1);
    Test.CompleteIdle = TRUE;
    pPoFxCompleteIdleCondition(Test.Handle, 0);
    ok_eq_ulong(EventCount(), n + 2);
    ok_event(n + 1, FxActive, 0, 0);

    /* An asynchronous activation comes from another thread */
    n = EventCount();
    pPoFxActivateComponent(Test.Handle, 1, PO_FX_FLAG_ASYNC_ONLY);
    ok(WaitForEvents(n + 1, 1000), "No asynchronous activation\n");
    Found = FALSE;
    for (i = n; i < EventCount() && i < MAX_EVENTS; i++)
    {
        if (Test.Events[i].Type == FxActive && Test.Events[i].Component == 1)
        {
            Found = TRUE;
            ok(Test.Events[i].Thread != KeGetCurrentThread(), "Asynchronous activation on the caller's thread\n");
        }
    }
    ok(Found, "Component 1 not active\n");

    /* Back to "power not required", with a short timeout and late completion */
    Test.CompleteNotRequired = FALSE;
    pPoFxSetDeviceIdleTimeout(Test.Handle, 0);
    SleepMs(600);
    n = EventCount();
    pPoFxIdleComponent(Test.Handle, 1, 0);
    pPoFxIdleComponent(Test.Handle, 0, 0);
    ok_eq_ulong(EventCount(), n + 3);
    ok_event(n, FxIdle, 1, 0);
    ok_event(n + 1, FxIdle, 0, 0);
    ok_event(n + 2, FxPowerNotRequired, 0, 0);

    /* No latency tolerated: component 1 stays in F0 */
    SleepMs(1100);
    ok_eq_ulong(EventCount(), n + 3);

    /* Activated before the power-down completed: power required only after it */
    pPoFxActivateComponent(Test.Handle, 0, 0);
    SleepMs(100);
    ok_eq_ulong(EventCount(), n + 3);
    pPoFxCompleteDevicePowerNotRequired(Test.Handle);
    ok_eq_ulong(EventCount(), n + 4);
    ok_event(n + 3, FxPowerRequired, 0, 0);
    pPoFxReportDevicePoweredOn(Test.Handle);
    ok(WaitForEvents(n + 5, 1000), "Not active after power-on\n");
    ok_event(n + 4, FxActive, 0, 0);

    /* Idle again, power not required, then unregister: it powers the device and activates everything first */
    Test.CompleteNotRequired = TRUE;
    Test.PowerOnInCallback = TRUE;
    SleepMs(600);
    n = EventCount();
    pPoFxIdleComponent(Test.Handle, 0, 0);
    ok_eq_ulong(EventCount(), n + 2);
    ok_event(n, FxIdle, 0, 0);
    ok_event(n + 1, FxPowerNotRequired, 0, 0);

    pPoFxUnregisterDevice(Test.Handle);
    ok_eq_ulong(EventCount(), n + 5);
    ok_event(n + 2, FxPowerRequired, 0, 0);
    ok(Test.Count >= n + 5 &&
       Test.Events[n + 3].Type == FxActive && Test.Events[n + 4].Type == FxActive &&
       Test.Events[n + 3].Component != Test.Events[n + 4].Component,
       "Both components not active before unregistering returned\n");

    /* Nothing after unregistering */
    n = EventCount();
    SleepMs(1200);
    ok_eq_ulong(EventCount(), n);

    for (i = 0; i < min(EventCount(), MAX_EVENTS); i++)
        trace("Event %lu: %d %lu %lu irql %u\n", i, Test.Events[i].Type, Test.Events[i].Component,
              Test.Events[i].State, Test.Events[i].Irql);
}

/* A kernel without PoFxRegisterDevice has no power framework (NT 6.1 and older) */
#define RESOLVE(Name) \
    do { \
        UNICODE_STRING _Name = RTL_CONSTANT_STRING(L ## #Name); \
        *(PVOID *)&p##Name = MmGetSystemRoutineAddress(&_Name); \
        ok(p##Name != NULL, #Name " is not exported\n"); \
        if (!p##Name) return; \
    } while (0)

START_TEST(PoFx)
{
    PDEVICE_OBJECT Pdo = NULL;
    NTSTATUS Status;

    {
        UNICODE_STRING Name = RTL_CONSTANT_STRING(L"PoFxRegisterDevice");
        pPoFxRegisterDevice = (PFN_REGISTER)MmGetSystemRoutineAddress(&Name);
        if (!pPoFxRegisterDevice)
        {
            skip(FALSE, "No power framework\n");
            return;
        }
    }
    RESOLVE(PoFxStartDevicePowerManagement);
    RESOLVE(PoFxUnregisterDevice);
    RESOLVE(PoFxActivateComponent);
    RESOLVE(PoFxIdleComponent);
    RESOLVE(PoFxCompleteIdleCondition);
    RESOLVE(PoFxCompleteIdleState);
    RESOLVE(PoFxCompleteDevicePowerNotRequired);
    RESOLVE(PoFxReportDevicePoweredOn);
    RESOLVE(PoFxSetDeviceIdleTimeout);

    KeInitializeSpinLock(&Test.Lock);

    /* A root-enumerated device of our own to register */
    Status = IoReportDetectedDevice(KmtDriverObject, InterfaceTypeUndefined, -1, -1,
                                    NULL, NULL, FALSE, &Pdo);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    TestRegistration(Pdo);

    RESOLVE(PoFxSetComponentLatency);
    RESOLVE(PoFxPowerControl);
    TestLifecycle(Pdo);
}

#else

START_TEST(PoFx)
{
    skip(FALSE, "The power framework is NT 6.2+\n");
}

#endif /* NTDDI_VERSION >= NTDDI_WIN8 */
