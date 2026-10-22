/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Missing headers (wdfcxbase.h): class extension callback types
 * COPYRIGHT:   2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 *              2026 ReactOS contributors
 */

/*
 * Microsoft does not publish wdfcxbase.h/wdfcx.h. The layouts below are
 * reconstructed from the MIT-licensed Windows-Driver-Frameworks sources
 * (FxVersionedStructures.h, FxCxPnpPowerCallbacks.hpp, PnpCallbacks.cpp,
 * FxCxDeviceInitApi.cpp) and from how the MIT-licensed NetAdapterCx sources
 * use them. A pre/post class extension callback has the same prototype as
 * the client callback it brackets; a "failed cleanup" callback takes the same
 * parameters and returns nothing.
 */

#ifndef _WDFCXBASE_H_
#define _WDFCXBASE_H_

#ifndef STDCALL
#define STDCALL __stdcall
#endif

/* File object handling */

typedef
_Function_class_(EVT_WDFCX_DEVICE_FILE_CREATE)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
STDCALL
EVT_WDFCX_DEVICE_FILE_CREATE(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_opt_ WDFFILEOBJECT FileObject);

typedef EVT_WDFCX_DEVICE_FILE_CREATE *PFN_WDFCX_DEVICE_FILE_CREATE;

typedef struct _WDFCX_FILEOBJECT_CONFIG {
    /* Size of this structure in bytes */
    ULONG Size;

    /* Event callback for create requests */
    PFN_WDFCX_DEVICE_FILE_CREATE EvtCxDeviceFileCreate;

    /* Event callback for close requests */
    PFN_WDF_FILE_CLOSE EvtFileClose;

    /* Event callback for cleanup requests */
    PFN_WDF_FILE_CLEANUP EvtFileCleanup;

    /* Whether create/cleanup/close requests are forwarded down the stack */
    WDF_TRI_STATE AutoForwardCleanupClose;

    /* How the framework associates a WDFFILEOBJECT with a WDM file object */
    WDF_FILEOBJECT_CLASS FileObjectClass;
} WDFCX_FILEOBJECT_CONFIG, *PWDFCX_FILEOBJECT_CONFIG;

FORCEINLINE
VOID
WDFCX_FILEOBJECT_CONFIG_INIT(
    _Out_ PWDFCX_FILEOBJECT_CONFIG FileEventCallbacks,
    _In_opt_ PFN_WDFCX_DEVICE_FILE_CREATE EvtCxDeviceFileCreate,
    _In_opt_ PFN_WDF_FILE_CLOSE EvtFileClose,
    _In_opt_ PFN_WDF_FILE_CLEANUP EvtFileCleanup)
{
    RtlZeroMemory(FileEventCallbacks, sizeof(WDFCX_FILEOBJECT_CONFIG));

    FileEventCallbacks->Size = sizeof(WDFCX_FILEOBJECT_CONFIG);
    FileEventCallbacks->EvtCxDeviceFileCreate = EvtCxDeviceFileCreate;
    FileEventCallbacks->EvtFileClose = EvtFileClose;
    FileEventCallbacks->EvtFileCleanup = EvtFileCleanup;
    FileEventCallbacks->FileObjectClass = WdfFileObjectWdfCannotUseFsContexts;
    FileEventCallbacks->AutoForwardCleanupClose = WdfUseDefault;
}

/* WDM IRP preprocessing */

typedef
_Function_class_(EVT_WDFCXDEVICE_WDM_IRP_PREPROCESS)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
STDCALL
EVT_WDFCXDEVICE_WDM_IRP_PREPROCESS(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp,
    _In_ PVOID DispatchContext);

typedef EVT_WDFCXDEVICE_WDM_IRP_PREPROCESS *PFN_WDFCXDEVICE_WDM_IRP_PREPROCESS;

/*
 * Pre/post PnP and power callbacks. They share the prototypes of the client
 * callbacks they wrap.
 */

#define WDFCX_DECLARE_PREPOST(_cxname, _clientname)                           \
    typedef EVT_WDF_DEVICE_##_clientname EVT_WDFCX_DEVICE_PRE_##_cxname;      \
    typedef EVT_WDF_DEVICE_##_clientname EVT_WDFCX_DEVICE_POST_##_cxname;     \
    typedef EVT_WDFCX_DEVICE_PRE_##_cxname *PFN_WDFCX_DEVICE_PRE_##_cxname;   \
    typedef EVT_WDFCX_DEVICE_POST_##_cxname *PFN_WDFCX_DEVICE_POST_##_cxname;

WDFCX_DECLARE_PREPOST(PREPARE_HARDWARE, PREPARE_HARDWARE)
WDFCX_DECLARE_PREPOST(RELEASE_HARDWARE, RELEASE_HARDWARE)
WDFCX_DECLARE_PREPOST(D0_ENTRY, D0_ENTRY)
WDFCX_DECLARE_PREPOST(D0_ENTRY_POST_HARDWARE_ENABLED, D0_ENTRY_POST_HARDWARE_ENABLED)
WDFCX_DECLARE_PREPOST(D0_EXIT, D0_EXIT)
WDFCX_DECLARE_PREPOST(D0_EXIT_PRE_HARDWARE_DISABLED, D0_EXIT_PRE_HARDWARE_DISABLED)
WDFCX_DECLARE_PREPOST(SURPRISE_REMOVAL, SURPRISE_REMOVAL)
WDFCX_DECLARE_PREPOST(SELF_MANAGED_IO_INIT, SELF_MANAGED_IO_INIT)
WDFCX_DECLARE_PREPOST(SELF_MANAGED_IO_RESTART, SELF_MANAGED_IO_RESTART)
WDFCX_DECLARE_PREPOST(SELF_MANAGED_IO_SUSPEND, SELF_MANAGED_IO_SUSPEND)
WDFCX_DECLARE_PREPOST(SELF_MANAGED_IO_FLUSH, SELF_MANAGED_IO_FLUSH)
WDFCX_DECLARE_PREPOST(SELF_MANAGED_IO_CLEANUP, SELF_MANAGED_IO_CLEANUP)
WDFCX_DECLARE_PREPOST(ARM_WAKE_FROM_S0, ARM_WAKE_FROM_S0)
WDFCX_DECLARE_PREPOST(DISARM_WAKE_FROM_S0, DISARM_WAKE_FROM_S0)
WDFCX_DECLARE_PREPOST(WAKE_FROM_S0_TRIGGERED, WAKE_FROM_S0_TRIGGERED)
WDFCX_DECLARE_PREPOST(ARM_WAKE_FROM_SX, ARM_WAKE_FROM_SX)
WDFCX_DECLARE_PREPOST(ARM_WAKE_FROM_SX_WITH_REASON, ARM_WAKE_FROM_SX_WITH_REASON)
WDFCX_DECLARE_PREPOST(DISARM_WAKE_FROM_SX, DISARM_WAKE_FROM_SX)
WDFCX_DECLARE_PREPOST(WAKE_FROM_SX_TRIGGERED, WAKE_FROM_SX_TRIGGERED)

#undef WDFCX_DECLARE_PREPOST

/* Self-managed I/O restart and suspend with the power state (KMDF 1.31) */

typedef
_Function_class_(EVT_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_RESTART_EX)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
STDCALL
EVT_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_RESTART_EX(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState);

typedef EVT_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_RESTART_EX EVT_WDFCX_DEVICE_POST_SELF_MANAGED_IO_RESTART_EX;
typedef EVT_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_RESTART_EX *PFN_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_RESTART_EX;
typedef EVT_WDFCX_DEVICE_POST_SELF_MANAGED_IO_RESTART_EX *PFN_WDFCX_DEVICE_POST_SELF_MANAGED_IO_RESTART_EX;

typedef
_Function_class_(EVT_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_SUSPEND_EX)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
STDCALL
EVT_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_SUSPEND_EX(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState);

typedef EVT_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_SUSPEND_EX EVT_WDFCX_DEVICE_POST_SELF_MANAGED_IO_SUSPEND_EX;
typedef EVT_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_SUSPEND_EX *PFN_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_SUSPEND_EX;
typedef EVT_WDFCX_DEVICE_POST_SELF_MANAGED_IO_SUSPEND_EX *PFN_WDFCX_DEVICE_POST_SELF_MANAGED_IO_SUSPEND_EX;

/* Called when the client callback fails after the pre callback succeeded */

#define WDFCX_DECLARE_CLEANUP(_name, _params)                                 \
    typedef                                                                   \
    _Function_class_(EVT_WDFCX_DEVICE_PRE_##_name##_FAILED_CLEANUP)           \
    _IRQL_requires_same_                                                      \
    _IRQL_requires_max_(PASSIVE_LEVEL)                                        \
    VOID                                                                      \
    STDCALL                                                                   \
    EVT_WDFCX_DEVICE_PRE_##_name##_FAILED_CLEANUP _params;                    \
    typedef EVT_WDFCX_DEVICE_PRE_##_name##_FAILED_CLEANUP                     \
        *PFN_WDFCX_DEVICE_PRE_##_name##_FAILED_CLEANUP;

WDFCX_DECLARE_CLEANUP(PREPARE_HARDWARE,
    (_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesRaw, _In_ WDFCMRESLIST ResourcesTranslated))
WDFCX_DECLARE_CLEANUP(D0_ENTRY,
    (_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState))
WDFCX_DECLARE_CLEANUP(D0_ENTRY_POST_HARDWARE_ENABLED,
    (_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState))
WDFCX_DECLARE_CLEANUP(SELF_MANAGED_IO_INIT,
    (_In_ WDFDEVICE Device))
WDFCX_DECLARE_CLEANUP(SELF_MANAGED_IO_RESTART,
    (_In_ WDFDEVICE Device))
WDFCX_DECLARE_CLEANUP(SELF_MANAGED_IO_RESTART_EX,
    (_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState))
WDFCX_DECLARE_CLEANUP(ARM_WAKE_FROM_S0,
    (_In_ WDFDEVICE Device))
WDFCX_DECLARE_CLEANUP(ARM_WAKE_FROM_SX,
    (_In_ WDFDEVICE Device))
WDFCX_DECLARE_CLEANUP(ARM_WAKE_FROM_SX_WITH_REASON,
    (_In_ WDFDEVICE Device, _In_ BOOLEAN DeviceWakeEnabled, _In_ BOOLEAN ChildrenArmedForWake))

#undef WDFCX_DECLARE_CLEANUP

/*
 * KMDF 1.29 introduced this structure with the fields up to
 * EvtCxDevicePostSelfManagedIoCleanup (WDFCX_PNPPOWER_EVENT_CALLBACKS_V1_29);
 * KMDF 1.31 appended the remaining ones.
 */
typedef struct _WDFCX_PNPPOWER_EVENT_CALLBACKS {
    /* Size of this structure in bytes */
    ULONG Size;

    PFN_WDFCX_DEVICE_PRE_PREPARE_HARDWARE                EvtCxDevicePrePrepareHardware;
    PFN_WDFCX_DEVICE_PRE_PREPARE_HARDWARE_FAILED_CLEANUP EvtCxDevicePrePrepareHardwareFailedCleanup;
    PFN_WDFCX_DEVICE_POST_PREPARE_HARDWARE               EvtCxDevicePostPrepareHardware;
    PFN_WDFCX_DEVICE_PRE_RELEASE_HARDWARE                EvtCxDevicePreReleaseHardware;
    PFN_WDFCX_DEVICE_POST_RELEASE_HARDWARE               EvtCxDevicePostReleaseHardware;
    PFN_WDFCX_DEVICE_PRE_D0_ENTRY                        EvtCxDevicePreD0Entry;
    PFN_WDFCX_DEVICE_PRE_D0_ENTRY_FAILED_CLEANUP         EvtCxDevicePreD0EntryFailedCleanup;
    PFN_WDFCX_DEVICE_POST_D0_ENTRY                       EvtCxDevicePostD0Entry;
    PFN_WDFCX_DEVICE_PRE_D0_EXIT                         EvtCxDevicePreD0Exit;
    PFN_WDFCX_DEVICE_POST_D0_EXIT                        EvtCxDevicePostD0Exit;

    PFN_WDFCX_DEVICE_PRE_SURPRISE_REMOVAL                EvtCxDevicePreSurpriseRemoval;
    PFN_WDFCX_DEVICE_POST_SURPRISE_REMOVAL               EvtCxDevicePostSurpriseRemoval;

    PFN_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_INIT            EvtCxDevicePreSelfManagedIoInit;
    PFN_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_INIT_FAILED_CLEANUP    EvtCxDevicePreSelfManagedIoInitFailedCleanup;
    PFN_WDFCX_DEVICE_POST_SELF_MANAGED_IO_INIT           EvtCxDevicePostSelfManagedIoInit;
    PFN_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_RESTART         EvtCxDevicePreSelfManagedIoRestart;
    PFN_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_RESTART_FAILED_CLEANUP EvtCxDevicePreSelfManagedIoRestartFailedCleanup;
    PFN_WDFCX_DEVICE_POST_SELF_MANAGED_IO_RESTART        EvtCxDevicePostSelfManagedIoRestart;
    PFN_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_SUSPEND         EvtCxDevicePreSelfManagedIoSuspend;
    PFN_WDFCX_DEVICE_POST_SELF_MANAGED_IO_SUSPEND        EvtCxDevicePostSelfManagedIoSuspend;
    PFN_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_FLUSH           EvtCxDevicePreSelfManagedIoFlush;
    PFN_WDFCX_DEVICE_POST_SELF_MANAGED_IO_FLUSH          EvtCxDevicePostSelfManagedIoFlush;
    PFN_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_CLEANUP         EvtCxDevicePreSelfManagedIoCleanup;
    PFN_WDFCX_DEVICE_POST_SELF_MANAGED_IO_CLEANUP        EvtCxDevicePostSelfManagedIoCleanup;

    /* KMDF 1.31 */
    PFN_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_RESTART_EX      EvtCxDevicePreSelfManagedIoRestartEx;
    PFN_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_RESTART_EX_FAILED_CLEANUP EvtCxDevicePreSelfManagedIoRestartExFailedCleanup;
    PFN_WDFCX_DEVICE_POST_SELF_MANAGED_IO_RESTART_EX     EvtCxDevicePostSelfManagedIoRestartEx;
    PFN_WDFCX_DEVICE_PRE_SELF_MANAGED_IO_SUSPEND_EX      EvtCxDevicePreSelfManagedIoSuspendEx;
    PFN_WDFCX_DEVICE_POST_SELF_MANAGED_IO_SUSPEND_EX     EvtCxDevicePostSelfManagedIoSuspendEx;
    PFN_WDFCX_DEVICE_PRE_D0_ENTRY_POST_HARDWARE_ENABLED  EvtCxDevicePreD0EntryPostHardwareEnabled;
    PFN_WDFCX_DEVICE_PRE_D0_ENTRY_POST_HARDWARE_ENABLED_FAILED_CLEANUP EvtCxDevicePreD0EntryPostHardwareEnabledFailedCleanup;
    PFN_WDFCX_DEVICE_POST_D0_ENTRY_POST_HARDWARE_ENABLED EvtCxDevicePostD0EntryPostHardwareEnabled;
    PFN_WDFCX_DEVICE_PRE_D0_EXIT_PRE_HARDWARE_DISABLED   EvtCxDevicePreD0ExitPreHardwareDisabled;
    PFN_WDFCX_DEVICE_POST_D0_EXIT_PRE_HARDWARE_DISABLED  EvtCxDevicePostD0ExitPreHardwareDisabled;
} WDFCX_PNPPOWER_EVENT_CALLBACKS, *PWDFCX_PNPPOWER_EVENT_CALLBACKS;

FORCEINLINE
VOID
WDFCX_PNPPOWER_EVENT_CALLBACKS_INIT(
    _Out_ PWDFCX_PNPPOWER_EVENT_CALLBACKS Callbacks)
{
    RtlZeroMemory(Callbacks, sizeof(WDFCX_PNPPOWER_EVENT_CALLBACKS));
    Callbacks->Size = sizeof(WDFCX_PNPPOWER_EVENT_CALLBACKS);
}

/* KMDF 1.31 */
typedef struct _WDFCX_POWER_POLICY_EVENT_CALLBACKS {
    /* Size of this structure in bytes */
    ULONG Size;

    PFN_WDFCX_DEVICE_PRE_ARM_WAKE_FROM_S0                EvtCxDevicePreArmWakeFromS0;
    PFN_WDFCX_DEVICE_PRE_ARM_WAKE_FROM_S0_FAILED_CLEANUP EvtCxDevicePreArmWakeFromS0FailedCleanup;
    PFN_WDFCX_DEVICE_POST_ARM_WAKE_FROM_S0               EvtCxDevicePostArmWakeFromS0;
    PFN_WDFCX_DEVICE_PRE_DISARM_WAKE_FROM_S0             EvtCxDevicePreDisarmWakeFromS0;
    PFN_WDFCX_DEVICE_POST_DISARM_WAKE_FROM_S0            EvtCxDevicePostDisarmWakeFromS0;
    PFN_WDFCX_DEVICE_PRE_WAKE_FROM_S0_TRIGGERED          EvtCxDevicePreWakeFromS0Triggered;
    PFN_WDFCX_DEVICE_POST_WAKE_FROM_S0_TRIGGERED         EvtCxDevicePostWakeFromS0Triggered;
    PFN_WDFCX_DEVICE_PRE_ARM_WAKE_FROM_SX                EvtCxDevicePreArmWakeFromSx;
    PFN_WDFCX_DEVICE_PRE_ARM_WAKE_FROM_SX_FAILED_CLEANUP EvtCxDevicePreArmWakeFromSxFailedCleanup;
    PFN_WDFCX_DEVICE_POST_ARM_WAKE_FROM_SX               EvtCxDevicePostArmWakeFromSx;
    PFN_WDFCX_DEVICE_PRE_ARM_WAKE_FROM_SX_WITH_REASON    EvtCxDevicePreArmWakeFromSxWithReason;
    PFN_WDFCX_DEVICE_PRE_ARM_WAKE_FROM_SX_WITH_REASON_FAILED_CLEANUP EvtCxDevicePreArmWakeFromSxWithReasonFailedCleanup;
    PFN_WDFCX_DEVICE_POST_ARM_WAKE_FROM_SX_WITH_REASON   EvtCxDevicePostArmWakeFromSxWithReason;
    PFN_WDFCX_DEVICE_PRE_DISARM_WAKE_FROM_SX             EvtCxDevicePreDisarmWakeFromSx;
    PFN_WDFCX_DEVICE_POST_DISARM_WAKE_FROM_SX            EvtCxDevicePostDisarmWakeFromSx;
    PFN_WDFCX_DEVICE_PRE_WAKE_FROM_SX_TRIGGERED          EvtCxDevicePreWakeFromSxTriggered;
    PFN_WDFCX_DEVICE_POST_WAKE_FROM_SX_TRIGGERED         EvtCxDevicePostWakeFromSxTriggered;
} WDFCX_POWER_POLICY_EVENT_CALLBACKS, *PWDFCX_POWER_POLICY_EVENT_CALLBACKS;

FORCEINLINE
VOID
WDFCX_POWER_POLICY_EVENT_CALLBACKS_INIT(
    _Out_ PWDFCX_POWER_POLICY_EVENT_CALLBACKS Callbacks)
{
    RtlZeroMemory(Callbacks, sizeof(WDFCX_POWER_POLICY_EVENT_CALLBACKS));
    Callbacks->Size = sizeof(WDFCX_POWER_POLICY_EVENT_CALLBACKS);
}

#endif /* _WDFCXBASE_H_ */
