/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device companion entry points (ReactOS stubs)
 * COPYRIGHT:   2026 ReactOS contributors
 */

/*
 * The device companion implementation is not part of the published WDF
 * sources and ReactOS has no UMDF companion host (see fxcompanionlibrary.hpp),
 * so no WDFCOMPANIONTARGET can exist. These are the pieces the rest of the
 * framework links against.
 */

#include "coreprivshared.hpp"
#include "fxpnpcallbacks.hpp"

NTSTATUS
FxPnpDeviceD0Entry::InvokeCompanionCallback(
    _In_ FxCompanionTarget* CompanionTarget
    )
{
    UNREFERENCED_PARAMETER(CompanionTarget);
    return STATUS_SUCCESS;
}

NTSTATUS
FxPnpDeviceD0Exit::InvokeCompanionCallback(
    _In_ FxCompanionTarget* CompanionTarget
    )
{
    UNREFERENCED_PARAMETER(CompanionTarget);
    return STATUS_SUCCESS;
}

NTSTATUS
FxPnpDevicePrepareHardware::InvokeCompanionCallback(
    _In_ FxCompanionTarget* CompanionTarget
    )
{
    UNREFERENCED_PARAMETER(CompanionTarget);
    return STATUS_SUCCESS;
}

NTSTATUS
FxPnpDeviceReleaseHardware::InvokeCompanionCallback(
    _In_ FxCompanionTarget* CompanionTarget
    )
{
    UNREFERENCED_PARAMETER(CompanionTarget);
    return STATUS_SUCCESS;
}

extern "C" {

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
WDFEXPORT(WdfCompanionTargetSendTaskSynchronously)(
    _In_
    PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_
    WDFCOMPANIONTARGET CompanionTarget,
    _In_
    USHORT TaskQueueIdentifier,
    _In_
    ULONG TaskOperationCode,
    _In_opt_
    PWDF_MEMORY_DESCRIPTOR InputBuffer,
    _In_opt_
    PWDF_MEMORY_DESCRIPTOR OutputBuffer,
    _In_opt_
    PWDF_TASK_SEND_OPTIONS TaskOptions,
    _Out_
    PULONG_PTR BytesReturned
    )
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(CompanionTarget);
    UNREFERENCED_PARAMETER(TaskQueueIdentifier);
    UNREFERENCED_PARAMETER(TaskOperationCode);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(TaskOptions);

    if (BytesReturned != NULL) {
        *BytesReturned = 0;
    }
    return STATUS_NOT_SUPPORTED;
}

_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
PEPROCESS
NTAPI
WDFEXPORT(WdfCompanionTargetWdmGetCompanionProcess)(
    _In_
    PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_
    WDFCOMPANIONTARGET CompanionTarget
    )
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(CompanionTarget);
    return NULL;
}

} // extern "C"
