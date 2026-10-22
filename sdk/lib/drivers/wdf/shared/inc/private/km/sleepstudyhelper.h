/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Sleep Study helper library (ReactOS stub)
 * COPYRIGHT:   2026 ReactOS contributors
 */

/*
 * Windows links WDF against a private Sleep Study helper library. ReactOS
 * has no Sleep Study, so these entry points do nothing and report
 * STATUS_NOT_SUPPORTED. FxLibraryGlobals.SleepStudyDisabled is forced on,
 * so the framework does not reach them in practice.
 */

#ifndef _SLEEPSTUDYHELPER_H_
#define _SLEEPSTUDYHELPER_H_

#define SLEEPSTUDY_POOL_TAG 'SSxF'

/* Characters kept from a PDO friendly name */
#define FRIENDLY_NAME_MAX_LENGTH 128

typedef struct _SS_LIBRARY_OPAQUE *SS_LIBRARY;
typedef struct _SS_COMPONENT_OPAQUE *SS_COMPONENT;

typedef enum _SSH_OBJECT_TYPE {
    SSH_PDO,
    SSH_FDO
} SSH_OBJECT_TYPE;

FORCEINLINE
NTSTATUS
SleepstudyHelper_Initialize(
    _Out_ SS_LIBRARY* Library,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    *Library = NULL;
    return STATUS_NOT_SUPPORTED;
}

FORCEINLINE
VOID
SleepstudyHelper_Uninitialize(
    _In_ SS_LIBRARY Library)
{
    UNREFERENCED_PARAMETER(Library);
}

FORCEINLINE
VOID
SleepstudyHelper_GenerateGuid(
    _In_ SSH_OBJECT_TYPE Type,
    _In_ ULONG64 Value,
    _Out_ GUID* Guid)
{
    UNREFERENCED_PARAMETER(Type);
    UNREFERENCED_PARAMETER(Value);
    RtlZeroMemory(Guid, sizeof(*Guid));
}

FORCEINLINE
NTSTATUS
SleepstudyHelper_GetPdoFriendlyName(
    _In_ PDEVICE_OBJECT Pdo,
    _Inout_ PUNICODE_STRING FriendlyName)
{
    UNREFERENCED_PARAMETER(Pdo);
    UNREFERENCED_PARAMETER(FriendlyName);
    return STATUS_NOT_SUPPORTED;
}

FORCEINLINE
NTSTATUS
SleepstudyHelper_RegisterComponentEx(
    _In_ SS_LIBRARY Library,
    _In_ GUID ParentGuid,
    _In_ GUID ComponentGuid,
    _In_ PUNICODE_STRING FriendlyName,
    _Out_ SS_COMPONENT* Component)
{
    UNREFERENCED_PARAMETER(Library);
    UNREFERENCED_PARAMETER(ParentGuid);
    UNREFERENCED_PARAMETER(ComponentGuid);
    UNREFERENCED_PARAMETER(FriendlyName);
    *Component = NULL;
    return STATUS_NOT_SUPPORTED;
}

FORCEINLINE
VOID
SleepstudyHelper_UnregisterComponent(
    _In_ SS_COMPONENT Component)
{
    UNREFERENCED_PARAMETER(Component);
}

FORCEINLINE
VOID
SleepstudyHelper_AcquireComponentLock(
    _In_ SS_COMPONENT Component,
    _Out_ PKIRQL Irql)
{
    UNREFERENCED_PARAMETER(Component);
    *Irql = KeGetCurrentIrql();
}

FORCEINLINE
VOID
SleepstudyHelper_ReleaseComponentLock(
    _In_ SS_COMPONENT Component,
    _In_ KIRQL Irql)
{
    UNREFERENCED_PARAMETER(Component);
    UNREFERENCED_PARAMETER(Irql);
}

FORCEINLINE
VOID
SleepstudyHelper_ComponentActive(
    _In_ SS_COMPONENT Component)
{
    UNREFERENCED_PARAMETER(Component);
}

FORCEINLINE
VOID
SleepstudyHelper_ComponentActiveLocked(
    _In_ SS_COMPONENT Component)
{
    UNREFERENCED_PARAMETER(Component);
}

FORCEINLINE
VOID
SleepstudyHelper_ComponentInactive(
    _In_ SS_COMPONENT Component)
{
    UNREFERENCED_PARAMETER(Component);
}

FORCEINLINE
VOID
SleepstudyHelper_ResetComponentsStartTime(
    _In_ SS_COMPONENT Component)
{
    UNREFERENCED_PARAMETER(Component);
}

#endif /* _SLEEPSTUDYHELPER_H_ */
