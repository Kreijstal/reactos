/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Version check of a binding KMDF client (WdfBindClientHelper)
 * COPYRIGHT:   2026 ReactOS contributors
 */

/*
 * The Windows framework links this helper from a library that is not part
 * of the published WDF sources. It decides whether a client may bind to
 * this framework and, for clients built with the KMDF 1.25+ stub (which
 * passes a WDF_BIND_INFO2), fills in the globals the client uses for
 * WDF_IS_FUNCTION_AVAILABLE() and WDF_IS_STRUCTURE_AVAILABLE():
 *
 *  - A client built for this or an older minor version always binds.
 *  - A client built for a newer minor version binds only if it uses
 *    WDF_BIND_INFO2 and its minimum required version is not newer than the
 *    framework. It then only sees the functions and structures that this
 *    framework provides.
 */

#include "fx.hpp"
#include "fxldr.h"

extern "C" {

NTSTATUS
WdfBindClientHelper(
    _Inout_ PWDF_BIND_INFO       BindInfo,
    _In_ WDF_MAJOR_VERSION       FxMajorVersion,
    _In_ WDF_MINOR_VERSION       FxMinorVersion
    )
{
    PWDF_BIND_INFO2 bindInfo2;
    WDF_MINOR_VERSION minimumRequired;
    BOOLEAN higherThanFramework;

    if (BindInfo->Version.Major != FxMajorVersion) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "WdfBindClientHelper: client major version %u, framework %u\n",
                   BindInfo->Version.Major, FxMajorVersion);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    higherThanFramework = (BindInfo->Version.Minor > FxMinorVersion);

    if (BindInfo->Size == sizeof(WDF_BIND_INFO)) {
        //
        // Old style client: the target version is also the minimum version
        //
        if (higherThanFramework) {
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                       "WdfBindClientHelper: client needs version %u.%u, "
                       "framework is %u.%u\n",
                       BindInfo->Version.Major, BindInfo->Version.Minor,
                       FxMajorVersion, FxMinorVersion);
            return STATUS_OBJECT_TYPE_MISMATCH;
        }
        return STATUS_SUCCESS;
    }

    if (BindInfo->Size != sizeof(WDF_BIND_INFO2)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "WdfBindClientHelper: unknown bind info size 0x%x\n",
                   BindInfo->Size);
        return STATUS_INVALID_PARAMETER;
    }

    bindInfo2 = (PWDF_BIND_INFO2)BindInfo;

    minimumRequired = BindInfo->Version.Minor;
    if (bindInfo2->MinimumVersionRequired != NULL &&
        *bindInfo2->MinimumVersionRequired < minimumRequired) {
        minimumRequired = *bindInfo2->MinimumVersionRequired;
    }

    if (minimumRequired > FxMinorVersion) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "WdfBindClientHelper: client needs at least version %u.%u, "
                   "framework is %u.%u\n",
                   BindInfo->Version.Major, minimumRequired,
                   FxMajorVersion, FxMinorVersion);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    if (bindInfo2->ClientVersionHigherThanFramework != NULL) {
        *bindInfo2->ClientVersionHigherThanFramework = higherThanFramework;
    }

    if (higherThanFramework) {
        //
        // The client was compiled against a newer framework. Give it our
        // function table and structure sizes so it can tell what exists.
        //
        BindInfo->FuncCount = WdfVersion.FuncCount;

        if (bindInfo2->FuncCountPtr != NULL) {
            *bindInfo2->FuncCountPtr = WdfVersion.FuncCount;
        }
        if (bindInfo2->StructCountPtr != NULL) {
            *bindInfo2->StructCountPtr = WdfVersion.StructCount;
        }
        if (bindInfo2->StructTable != NULL) {
            *bindInfo2->StructTable = (WDF_STRUCT_INFO)&WdfVersion.Structures;
        }
    }
    else if (bindInfo2->FuncCountPtr != NULL) {
        *bindInfo2->FuncCountPtr = BindInfo->FuncCount;
    }

    return STATUS_SUCCESS;
}

} // extern "C"
