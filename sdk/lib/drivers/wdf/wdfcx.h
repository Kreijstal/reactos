/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Missing headers (wdfcx.h)
 * COPYRIGHT:   2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 *              2026 ReactOS contributors
 */

#ifndef _WDFCX_H_
#define _WDFCX_H_

#include "kmdf/inc/private/wdfldr.h"

#include "wdfcxbase.h"

typedef PWDFCXDEVICE_INIT
(NTAPI *PFN_WDFCXDEVICEINITALLOCATE)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFDEVICE_INIT DeviceInit);

typedef NTSTATUS
(NTAPI *PFN_WDFCXDEVICEINITASSIGNWDMIRPPREPROCESSCALLBACK) (
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PFN_WDFCXDEVICE_WDM_IRP_PREPROCESS EvtCxDeviceWdmIrpPreprocess,
    _In_ UCHAR MajorFunction,
    _When_(NumMinorFunctions > 0, _In_reads_bytes_(NumMinorFunctions))
    _When_(NumMinorFunctions == 0, _In_opt_)
    PUCHAR MinorFunctions,
    _In_ ULONG NumMinorFunctions);

typedef VOID
(NTAPI *PFN_WDFCXDEVICEINITSETIOINCALLERCONTEXTCALLBACK)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PFN_WDF_IO_IN_CALLER_CONTEXT EvtIoInCallerContext);

typedef VOID
(NTAPI *PFN_WDFCXDEVICEINITSETREQUESTATTRIBUTES)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PWDF_OBJECT_ATTRIBUTES RequestAttributes);

typedef VOID
(NTAPI *PFN_WDFCXDEVICEINITSETFILEOBJECTCONFIG)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PWDFCX_FILEOBJECT_CONFIG CxFileObjectConfig,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES FileObjectAttributes);

typedef VOID
(NTAPI *PFN_WDFCXVERIFIERKEBUGCHECK)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_opt_ WDFOBJECT Object,
    _In_ ULONG BugCheckCode,
    _In_ ULONG_PTR BugCheckParameter1,
    _In_ ULONG_PTR BugCheckParameter2,
    _In_ ULONG_PTR BugCheckParameter3,
    _In_ ULONG_PTR BugCheckParameter4);

typedef WDFIOTARGET
(NTAPI *PFN_WDFDEVICEGETSELFIOTARGET)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device);

typedef VOID
(NTAPI *PFN_WDFDEVICEINITALLOWSELFIOTARGET)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFDEVICE_INIT DeviceInit);

typedef NTSTATUS
(NTAPI *PFN_WDFIOTARGETSELFASSIGNDEFAULTIOQUEUE)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFIOTARGET IoTarget,
    _In_ WDFQUEUE Queue);

/* KMDF 1.19 */
typedef VOID
(NTAPI *PFN_WDFCXDEVICEINITSETPNPPOWEREVENTCALLBACKS)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PWDFCX_PNPPOWER_EVENT_CALLBACKS CxPnpPowerCallbacks);

/* KMDF 1.31 */
typedef NTSTATUS
(NTAPI *PFN_WDFCXDEVICEINITALLOCATECONTEXT)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ PWDF_OBJECT_ATTRIBUTES ContextAttributes,
    _Outptr_opt_ PVOID* Context);

typedef PVOID
(NTAPI *PFN_WDFCXDEVICEINITGETTYPEDCONTEXTWORKER)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ PCWDF_OBJECT_CONTEXT_TYPE_INFO TypeInfo);

typedef VOID
(NTAPI *PFN_WDFCXDEVICEINITSETPOWERPOLICYEVENTCALLBACKS)(
    _In_ PWDF_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PWDFCX_POWER_POLICY_EVENT_CALLBACKS CxPowerPolicyCallbacks);

/*
 * Client-side wrappers. The framework does not define them; it only needs
 * the prototypes above.
 */
#if !defined(FX_CORE_MODE)

_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
PWDFCXDEVICE_INIT
WdfCxDeviceInitAllocate(
    _In_ PWDFDEVICE_INIT DeviceInit)
{
    return ((PFN_WDFCXDEVICEINITALLOCATE) WdfFunctions[WdfCxDeviceInitAllocateTableIndex])(WdfDriverGlobals, DeviceInit);
}

_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NTSTATUS
WdfCxDeviceInitAssignWdmIrpPreprocessCallback(
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PFN_WDFCXDEVICE_WDM_IRP_PREPROCESS EvtCxDeviceWdmIrpPreprocess,
    _In_ UCHAR MajorFunction,
    _When_(NumMinorFunctions > 0, _In_reads_bytes_(NumMinorFunctions))
    _When_(NumMinorFunctions == 0, _In_opt_)
    PUCHAR MinorFunctions,
    _In_ ULONG NumMinorFunctions)
{
    return ((PFN_WDFCXDEVICEINITASSIGNWDMIRPPREPROCESSCALLBACK) WdfFunctions[WdfCxDeviceInitAssignWdmIrpPreprocessCallbackTableIndex])(WdfDriverGlobals, CxDeviceInit, EvtCxDeviceWdmIrpPreprocess, MajorFunction, MinorFunctions, NumMinorFunctions);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
WdfCxDeviceInitSetIoInCallerContextCallback(
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PFN_WDF_IO_IN_CALLER_CONTEXT EvtIoInCallerContext)
{
    ((PFN_WDFCXDEVICEINITSETIOINCALLERCONTEXTCALLBACK) WdfFunctions[WdfCxDeviceInitSetIoInCallerContextCallbackTableIndex])(WdfDriverGlobals, CxDeviceInit, EvtIoInCallerContext);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
WdfCxDeviceInitSetRequestAttributes(
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PWDF_OBJECT_ATTRIBUTES RequestAttributes)
{
    ((PFN_WDFCXDEVICEINITSETREQUESTATTRIBUTES) WdfFunctions[WdfCxDeviceInitSetRequestAttributesTableIndex])(WdfDriverGlobals, CxDeviceInit, RequestAttributes);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
WdfCxDeviceInitSetFileObjectConfig(
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PWDFCX_FILEOBJECT_CONFIG CxFileObjectConfig,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES FileObjectAttributes)
{
    ((PFN_WDFCXDEVICEINITSETFILEOBJECTCONFIG) WdfFunctions[WdfCxDeviceInitSetFileObjectConfigTableIndex])(WdfDriverGlobals, CxDeviceInit, CxFileObjectConfig, FileObjectAttributes);
}

FORCEINLINE
VOID
WdfCxVerifierKeBugCheck(
    _In_opt_ WDFOBJECT Object,
    _In_ ULONG BugCheckCode,
    _In_ ULONG_PTR BugCheckParameter1,
    _In_ ULONG_PTR BugCheckParameter2,
    _In_ ULONG_PTR BugCheckParameter3,
    _In_ ULONG_PTR BugCheckParameter4)
{
    ((PFN_WDFCXVERIFIERKEBUGCHECK) WdfFunctions[WdfCxVerifierKeBugCheckTableIndex])(WdfDriverGlobals, Object, BugCheckCode, BugCheckParameter1, BugCheckParameter2, BugCheckParameter3, BugCheckParameter4);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
WdfCxDeviceInitSetPnpPowerEventCallbacks(
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PWDFCX_PNPPOWER_EVENT_CALLBACKS CxPnpPowerCallbacks)
{
    ((PFN_WDFCXDEVICEINITSETPNPPOWEREVENTCALLBACKS) WdfFunctions[WdfCxDeviceInitSetPnpPowerEventCallbacksTableIndex])(WdfDriverGlobals, CxDeviceInit, CxPnpPowerCallbacks);
}

_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NTSTATUS
WdfCxDeviceInitAllocateContext(
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ PWDF_OBJECT_ATTRIBUTES ContextAttributes,
    _Outptr_opt_ PVOID* Context)
{
    return ((PFN_WDFCXDEVICEINITALLOCATECONTEXT) WdfFunctions[WdfCxDeviceInitAllocateContextTableIndex])(WdfDriverGlobals, DeviceInit, ContextAttributes, Context);
}

_IRQL_requires_max_(DISPATCH_LEVEL+1)
FORCEINLINE
PVOID
WdfCxDeviceInitGetTypedContextWorker(
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ PCWDF_OBJECT_CONTEXT_TYPE_INFO TypeInfo)
{
    return ((PFN_WDFCXDEVICEINITGETTYPEDCONTEXTWORKER) WdfFunctions[WdfCxDeviceInitGetTypedContextWorkerTableIndex])(WdfDriverGlobals, DeviceInit, TypeInfo);
}

#define WdfCxDeviceInitGetTypedContext(_deviceinit, _type)                   \
    ((WDF_TYPE_NAME_POINTER_TYPE(_type))                                      \
        WdfCxDeviceInitGetTypedContextWorker(                                 \
            (_deviceinit), WDF_GET_CONTEXT_TYPE_INFO(_type)->UniqueType))

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
WdfCxDeviceInitSetPowerPolicyEventCallbacks(
    _In_ PWDFCXDEVICE_INIT CxDeviceInit,
    _In_ PWDFCX_POWER_POLICY_EVENT_CALLBACKS CxPowerPolicyCallbacks)
{
    ((PFN_WDFCXDEVICEINITSETPOWERPOLICYEVENTCALLBACKS) WdfFunctions[WdfCxDeviceInitSetPowerPolicyEventCallbacksTableIndex])(WdfDriverGlobals, CxDeviceInit, CxPowerPolicyCallbacks);
}

#endif /* !FX_CORE_MODE */

/* Class Extension support */
typedef NTSTATUS (NTAPI *PFN_WDF_CLASS_EXTENSIONIN_BIND)(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals);

typedef VOID (NTAPI *PFN_WDF_CLASS_EXTENSIONIN_UNBIND)(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals);

typedef PVOID (NTAPI *PFN_WDF_CLASS_EXPORT)(VOID);

typedef NTSTATUS (NTAPI *PFN_WDF_CLASS_LIBRARY_INITIALIZE)(VOID);

typedef VOID (NTAPI *PFN_WDF_CLASS_LIBRARY_DEINITIALIZE)(VOID);

typedef NTSTATUS (NTAPI *PFN_WDF_CLASS_LIBRARY_BIND_CLIENT)(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _Inout_ PWDF_COMPONENT_GLOBALS* ClientGlobals);

typedef VOID (NTAPI *PFN_WDF_CLASS_LIBRARY_UNBIND_CLIENT)(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _Inout_ PWDF_COMPONENT_GLOBALS* ClientGlobals);

typedef NTSTATUS (NTAPI *PFN_WDF_CLIENT_BIND_CLASS)(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals);

typedef VOID (NTAPI *PFN_WDF_CLIENT_UNBIND_CLASS)(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ComponentGlobals);

#endif // _WDFCX_H_
