/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite for the calling convention of the power framework callback types
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * The WDK declares the PO_FX callback function types, and a few other kernel
 * callback types, without a calling convention; kernel-mode code is built
 * with /Gz, so they are stdcall. A ReactOS header without NTAPI makes them
 * cdecl on x86, and the kernel would then call a binary driver's stdcall
 * callback with the wrong stack cleanup. This file implements every callback
 * as NTAPI through the function type: on x86 that does not compile when the
 * type has another convention. The run-time part calls each one through the
 * pointer types of PO_FX_DEVICE_V1, as the power framework does.
 *
 * The types exist from NTDDI_WIN8 on; this file asks for them in every
 * configuration, the other tests keep the configured version.
 */

#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x06020000

#include <kmt_test.h>

static PO_FX_COMPONENT_ACTIVE_CONDITION_CALLBACK TestActiveCondition;
static PO_FX_COMPONENT_IDLE_CONDITION_CALLBACK TestIdleCondition;
static PO_FX_COMPONENT_IDLE_STATE_CALLBACK TestIdleState;
static PO_FX_DEVICE_POWER_REQUIRED_CALLBACK TestPowerRequired;
static PO_FX_DEVICE_POWER_NOT_REQUIRED_CALLBACK TestPowerNotRequired;
static PO_FX_POWER_CONTROL_CALLBACK TestPowerControl;
static PO_FX_COMPONENT_CRITICAL_TRANSITION_CALLBACK TestCriticalTransition;
static PO_FX_DIRECTED_POWER_UP_CALLBACK TestDirectedPowerUp;
static PO_FX_DIRECTED_POWER_DOWN_CALLBACK TestDirectedPowerDown;

#if defined(__GNUC__) && !defined(__cplusplus)
/* Pointer types: the same check where the compiler can compare types */
#define CHECK_TYPE(Type, Expected) C_ASSERT(__builtin_types_compatible_p(Type, Expected))
CHECK_TYPE(PFSRTL_EXTRA_CREATE_PARAMETER_CLEANUP_CALLBACK, VOID (NTAPI *)(PVOID, LPCGUID));
CHECK_TYPE(PFN_FSRTLTEARDOWNPERSTREAMCONTEXTS, VOID (NTAPI *)(PFSRTL_ADVANCED_FCB_HEADER));
CHECK_TYPE(PREAD_ACPI_REGISTER, USHORT (NTAPI *)(ACPI_REG_TYPE, ULONG));
CHECK_TYPE(PWRITE_ACPI_REGISTER, VOID (NTAPI *)(ACPI_REG_TYPE, ULONG, USHORT));
CHECK_TYPE(PHAL_FREE_PORT_RANGE, VOID (NTAPI *)(USHORT));
CHECK_TYPE(PDEBUG_PRINT_CALLBACK, VOID (NTAPI *)(PSTRING, ULONG, ULONG));
CHECK_TYPE(PFN_RTL_IS_NTDDI_VERSION_AVAILABLE, BOOLEAN (NTAPI *)(ULONG));
CHECK_TYPE(PFN_RTL_IS_SERVICE_PACK_VERSION_INSTALLED, BOOLEAN (NTAPI *)(ULONG));
#endif

#define TEST_CONTEXT ((PVOID)(ULONG_PTR)0x5a5a1234)
static const GUID TestGuid = { 0x1, 0x2, 0x3, { 4, 5, 6, 7, 8, 9, 10, 11 } };

static ULONG Calls;
static ULONG LastComponent;
static ULONG LastValue;

static
VOID
NTAPI
TestActiveCondition(
    _In_ PVOID Context,
    _In_ ULONG Component)
{
    ok_eq_pointer(Context, TEST_CONTEXT);
    LastComponent = Component;
    Calls++;
}

static
VOID
NTAPI
TestIdleCondition(
    _In_ PVOID Context,
    _In_ ULONG Component)
{
    ok_eq_pointer(Context, TEST_CONTEXT);
    LastComponent = Component;
    Calls++;
}

static
VOID
NTAPI
TestIdleState(
    _In_ PVOID Context,
    _In_ ULONG Component,
    _In_ ULONG State)
{
    ok_eq_pointer(Context, TEST_CONTEXT);
    LastComponent = Component;
    LastValue = State;
    Calls++;
}

static
VOID
NTAPI
TestPowerRequired(
    _In_ PVOID Context)
{
    ok_eq_pointer(Context, TEST_CONTEXT);
    Calls++;
}

static
VOID
NTAPI
TestPowerNotRequired(
    _In_ PVOID Context)
{
    ok_eq_pointer(Context, TEST_CONTEXT);
    Calls++;
}

static
NTSTATUS
NTAPI
TestPowerControl(
    _In_ PVOID DeviceContext,
    _In_ LPCGUID PowerControlCode,
    _In_reads_bytes_opt_(InBufferSize) PVOID InBuffer,
    _In_ SIZE_T InBufferSize,
    _Out_writes_bytes_opt_(OutBufferSize) PVOID OutBuffer,
    _In_ SIZE_T OutBufferSize,
    _Out_opt_ PSIZE_T BytesReturned)
{
    ok_eq_pointer(DeviceContext, TEST_CONTEXT);
    ok_eq_pointer(PowerControlCode, &TestGuid);
    ok_eq_pointer(InBuffer, NULL);
    ok_eq_size(InBufferSize, 3);
    ok_eq_pointer(OutBuffer, NULL);
    ok_eq_size(OutBufferSize, 5);
    if (BytesReturned)
        *BytesReturned = 7;
    Calls++;
    return STATUS_WAIT_2;
}

static
VOID
NTAPI
TestCriticalTransition(
    _In_ PVOID Context,
    _In_ ULONG Component,
    _In_ BOOLEAN Active)
{
    ok_eq_pointer(Context, TEST_CONTEXT);
    LastComponent = Component;
    LastValue = Active;
    Calls++;
}

static
VOID
NTAPI
TestDirectedPowerUp(
    _In_ PVOID Context,
    _In_ ULONG Flags)
{
    ok_eq_pointer(Context, TEST_CONTEXT);
    LastValue = Flags;
    Calls++;
}

static
VOID
NTAPI
TestDirectedPowerDown(
    _In_ PVOID Context,
    _In_ ULONG Flags)
{
    ok_eq_pointer(Context, TEST_CONTEXT);
    LastValue = Flags;
    Calls++;
}

START_TEST(PoFxCallbacks)
{
    PO_FX_DEVICE_V1 Device;
    PPO_FX_COMPONENT_CRITICAL_TRANSITION_CALLBACK CriticalTransition = TestCriticalTransition;
    PPO_FX_DIRECTED_POWER_UP_CALLBACK DirectedPowerUp = TestDirectedPowerUp;
    PPO_FX_DIRECTED_POWER_DOWN_CALLBACK DirectedPowerDown = TestDirectedPowerDown;
    SIZE_T BytesReturned = 0;
    NTSTATUS Status;

    RtlZeroMemory(&Device, sizeof(Device));
    Device.Version = PO_FX_VERSION_V1;
    Device.ComponentActiveConditionCallback = TestActiveCondition;
    Device.ComponentIdleConditionCallback = TestIdleCondition;
    Device.ComponentIdleStateCallback = TestIdleState;
    Device.DevicePowerRequiredCallback = TestPowerRequired;
    Device.DevicePowerNotRequiredCallback = TestPowerNotRequired;
    Device.PowerControlCallback = TestPowerControl;
    Device.DeviceContext = TEST_CONTEXT;

    Device.ComponentActiveConditionCallback(Device.DeviceContext, 3);
    ok_eq_ulong(LastComponent, 3UL);
    Device.ComponentIdleConditionCallback(Device.DeviceContext, 4);
    ok_eq_ulong(LastComponent, 4UL);
    Device.ComponentIdleStateCallback(Device.DeviceContext, 5, 2);
    ok_eq_ulong(LastComponent, 5UL);
    ok_eq_ulong(LastValue, 2UL);
    Device.DevicePowerRequiredCallback(Device.DeviceContext);
    Device.DevicePowerNotRequiredCallback(Device.DeviceContext);
    Status = Device.PowerControlCallback(Device.DeviceContext, &TestGuid,
                                         NULL, 3, NULL, 5, &BytesReturned);
    ok_eq_hex(Status, STATUS_WAIT_2);
    ok_eq_size(BytesReturned, 7);
    CriticalTransition(Device.DeviceContext, 6, TRUE);
    ok_eq_ulong(LastComponent, 6UL);
    ok_eq_ulong(LastValue, 1UL);
    DirectedPowerUp(Device.DeviceContext, 0x11);
    ok_eq_ulong(LastValue, 0x11UL);
    DirectedPowerDown(Device.DeviceContext, 0x22);
    ok_eq_ulong(LastValue, 0x22UL);

    ok_eq_ulong(Calls, 9UL);
}
