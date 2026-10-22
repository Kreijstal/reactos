/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Device companion target object (ReactOS stub)
 * COPYRIGHT:   2026 ReactOS contributors
 */

/*
 * See fxcompanionlibrary.hpp: no companion is ever required on ReactOS, so
 * this object cannot be initialized. It only exists so the object table and
 * the PnP code that would talk to a companion still build.
 */

#pragma once

class FxCompanionTarget : public FxNonPagedObject {
public:
    FxCompanionTarget(
        _In_ PFX_DRIVER_GLOBALS FxDriverGlobals,
        _In_ USHORT ObjectSize
        ) :
        FxNonPagedObject(FX_TYPE_COMPANION_TARGET, ObjectSize, FxDriverGlobals)
    {
    }

    _Must_inspect_result_
    NTSTATUS
    Init(
        _In_ FxDevice* Device
        )
    {
        UNREFERENCED_PARAMETER(Device);
        return STATUS_NOT_SUPPORTED;
    }

    VOID
    QueryPnPDeviceStateNotification(
        VOID
        )
    {
    }

    _Must_inspect_result_
    NTSTATUS
    HandleQueryInterfaceForSecureDriver(
        _Inout_ FxIrp* Irp,
        _Out_ PBOOLEAN CompleteRequest
        )
    {
        UNREFERENCED_PARAMETER(Irp);
        *CompleteRequest = FALSE;
        return STATUS_NOT_SUPPORTED;
    }
};
