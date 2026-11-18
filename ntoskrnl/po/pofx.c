/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Runtime power management framework (PoFx)
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * A driver registers its device with the power framework as a set of
 * components. Each component is either active (in use) or idle, as counted
 * by the activation references the driver takes and drops with
 * PoFxActivateComponent and PoFxIdleComponent, and is in one of its F-states
 * (F0 = fully on). The device as a whole either requires power (it must stay
 * in D0) or not (the driver may send it to a low-power D-state).
 *
 * Every transition is a handshake with the driver:
 *  - a component that loses its last activation reference goes through the
 *    idle condition callback, completed with PoFxCompleteIdleCondition;
 *  - an idle component that stays idle long enough is moved to the deepest
 *    F-state its residency, latency and wake constraints allow, through the
 *    idle state callback, completed with PoFxCompleteIdleState;
 *  - once every component is idle and the device idle timeout has elapsed,
 *    the device power not required callback lets the driver power the device
 *    down; it completes with PoFxCompleteDevicePowerNotRequired;
 *  - a component activation on a device whose power is not required goes
 *    through the device power required callback first; the driver powers the
 *    device up and reports that with PoFxReportDevicePoweredOn;
 *  - an activated component goes back to F0 (idle state callback) and then
 *    through the active condition callback.
 *
 * On Windows these decisions are the power framework's own; a platform
 * extension plug-in (PEP) that claims a device is only told about them and
 * may add constraints. ReactOS has no PEP, which is what a classic ACPI
 * system without S0 low-power idle looks like: nothing is ever vetoed, and
 * PoFxPowerControl has nobody to talk to.
 *
 * As on Windows, the work goes on in the thread whose call made it necessary
 * (the idle timer's DPC for the device idle timeout, a worker for
 * PO_FX_FLAG_ASYNC_ONLY), and a driver may call back into the power framework
 * from inside a callback. Here the callbacks for one device are serialized:
 * they are made one at a time, with no lock held; the idle condition and
 * device power not required callbacks at DISPATCH_LEVEL, like Windows.
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* TYPES *********************************************************************/

#define TAG_PO_FX               'xFoP'
#define POP_FX_SIGNATURE        'DxFP'

/* A component idled again within this time is kept active for a while */
#define POP_FX_DEBOUNCE_THRESHOLD   (500 * 10000ULL)
#define POP_FX_DEBOUNCE_TIMEOUT     (1000 * 10000LL)
/* How often idle components are checked for a deeper F-state */
#define POP_FX_IDLE_STATE_PERIOD    500

/* INTERNAL_POWER_ERROR (0xA0) subcodes, as on Windows */
#define POP_FX_BUGCHECK_ALREADY_REGISTERED  0x600
#define POP_FX_BUGCHECK_SX_COUNT_MISMATCH   0x602
#define POP_FX_BUGCHECK_IDLE_UNDERFLOW      0x608
#define POP_FX_BUGCHECK_COMPLETE_UNDERFLOW  0x613
#define POP_FX_BUGCHECK_BAD_REQUEST         0x614
#define POP_FX_BUGCHECK_BAD_ACTIVATION      0x615
#define POP_FX_BUGCHECK_BAD_IDLE_STATE      0x616

typedef enum _POP_FX_COMPONENT_CONDITION
{
    /* In use: the active condition callback was made */
    PopFxComponentActive,
    /* The idle condition callback was made, PoFxCompleteIdleCondition is due */
    PopFxComponentIdling,
    /* Idle: PoFxCompleteIdleCondition was called */
    PopFxComponentIdle,
} POP_FX_COMPONENT_CONDITION;

typedef enum _POP_FX_DEVICE_POWER
{
    /* The device must be in D0 */
    PopFxDevicePowerRequired,
    /* The not required callback was made, PoFxCompleteDevicePowerNotRequired is due */
    PopFxDevicePowerNotRequiring,
    /* The driver may keep the device in a low-power D-state */
    PopFxDevicePowerNotRequired,
    /* The required callback was made, PoFxReportDevicePoweredOn is due */
    PopFxDevicePowerRequiring,
} POP_FX_DEVICE_POWER;

typedef struct _POP_FX_COMPONENT
{
    GUID Id;
    ULONGLONG Flags;
    /* Activation references: PoFxActivateComponent minus PoFxIdleComponent */
    ULONG ActiveCount;
    POP_FX_COMPONENT_CONDITION Condition;
    /* Idled again too soon after the last idle request: idles from the debounce timer */
    BOOLEAN Resident;
    /* An idle state callback was made, PoFxCompleteIdleState is due */
    BOOLEAN IdleStatePending;
    ULONG IdleState;
    ULONG TargetIdleState;
    /* Interrupt time of the last PoFxIdleComponent that dropped the last reference */
    ULONGLONG LastIdleRequest;
    /* Interrupt time the component completed its idle condition */
    ULONGLONG IdleSince;
    /* PoFxSetComponentLatency / Residency / Wake */
    ULONGLONG LatencyTolerance;
    ULONGLONG ExpectedResidency;
    BOOLEAN WakeArmed;
    ULONG IdleStateCount;
    ULONG DeepestWakeableIdleState;
    PPO_FX_COMPONENT_IDLE_STATE IdleStates;
} POP_FX_COMPONENT, *PPOP_FX_COMPONENT;

typedef struct _POP_FX_DEVICE
{
    ULONG Signature;
    LIST_ENTRY Link;
    PDEVICE_OBJECT Pdo;
    ULONG Version;
    ULONGLONG Flags;
    PPO_FX_COMPONENT_ACTIVE_CONDITION_CALLBACK ComponentActiveConditionCallback;
    PPO_FX_COMPONENT_IDLE_CONDITION_CALLBACK ComponentIdleConditionCallback;
    PPO_FX_COMPONENT_IDLE_STATE_CALLBACK ComponentIdleStateCallback;
    PPO_FX_DEVICE_POWER_REQUIRED_CALLBACK DevicePowerRequiredCallback;
    PPO_FX_DEVICE_POWER_NOT_REQUIRED_CALLBACK DevicePowerNotRequiredCallback;
    PPO_FX_POWER_CONTROL_CALLBACK PowerControlCallback;
    PPO_FX_DIRECTED_POWER_UP_CALLBACK DirectedPowerUpCallback;
    PPO_FX_DIRECTED_POWER_DOWN_CALLBACK DirectedPowerDownCallback;
    PVOID DeviceContext;

    KSPIN_LOCK Lock;
    /* The registration and a queued worker */
    LONG RefCount;
    /* PoFxStartDevicePowerManagement was called */
    BOOLEAN Started;
    /* PoFxUnregisterDevice is done waiting for the components: no more callbacks */
    BOOLEAN Unregistering;
    /* A thread is making the callbacks for this device; RunAgain: look again */
    BOOLEAN Running;
    BOOLEAN RunAgain;
    PKTHREAD RunningThread;
    BOOLEAN WorkQueued;
    BOOLEAN IdleTimerArmed;
    BOOLEAN DebounceTimerArmed;
    /* The debounce timer fired: idle the components kept active long enough */
    BOOLEAN DebounceRelease;
    BOOLEAN IdleStateTimerArmed;
    /* The idle state timer fired: look for deeper F-states */
    BOOLEAN IdleStateCheck;
    POP_FX_DEVICE_POWER DevicePower;
    /* Activations taken for system sleep, and sleep IRPs completed since */
    ULONG SxActivations;
    ULONG SleepCount;
    /* Components not idle (PopFxComponentIdle) */
    ULONG NotIdleCount;
    /* Interrupt time the last component completed its idle condition */
    ULONGLONG AllIdleSince;
    /* PoFxSetDeviceIdleTimeout, 100 ns units */
    ULONGLONG IdleTimeout;
    KTIMER IdleTimer;
    KDPC IdleDpc;
    KTIMER DebounceTimer;
    KDPC DebounceDpc;
    KTIMER IdleStateTimer;
    KDPC IdleStateDpc;
    WORK_QUEUE_ITEM WorkItem;
    /* Set, under the lock, whenever the state moved on */
    KEVENT StateEvent;
    ULONG ComponentCount;
    POP_FX_COMPONENT Components[ANYSIZE_ARRAY];
} POP_FX_DEVICE, *PPOP_FX_DEVICE;

/* What the state of a device asks to be done next, outside the lock */
typedef enum _POP_FX_ACTION
{
    PopFxActionNone,
    PopFxActionActive,
    PopFxActionIdle,
    PopFxActionIdleState,
    PopFxActionPowerRequired,
    PopFxActionPowerNotRequired,
} POP_FX_ACTION;

/* GLOBALS *******************************************************************/

static KSPIN_LOCK PopFxDeviceListLock;
static LIST_ENTRY PopFxDeviceList;

/* PRIVATE FUNCTIONS *********************************************************/

static
PPOP_FX_DEVICE
PopFxGetDevice(
    _In_ POHANDLE Handle)
{
    PPOP_FX_DEVICE Device = (PPOP_FX_DEVICE)Handle;

    ASSERT(Device != NULL && Device->Signature == POP_FX_SIGNATURE);
    return Device;
}

static
VOID
PopFxDereferenceDevice(
    _In_ PPOP_FX_DEVICE Device)
{
    if (InterlockedDecrement(&Device->RefCount) == 0)
    {
        Device->Signature = 0;
        ObDereferenceObject(Device->Pdo);
        ExFreePoolWithTag(Device, TAG_PO_FX);
    }
}

/* Has the worker run the device. Lock held. */
static
VOID
PopFxQueueWork(
    _In_ PPOP_FX_DEVICE Device)
{
    if (!Device->WorkQueued && !Device->Unregistering)
    {
        Device->WorkQueued = TRUE;
        InterlockedIncrement(&Device->RefCount);
        ExQueueWorkItem(&Device->WorkItem, CriticalWorkQueue);
    }
}

static
VOID
PopFxCheckRequest(
    _In_ PPOP_FX_DEVICE Device,
    _In_ ULONG Component,
    _In_ ULONG Flags)
{
    if ((Flags & (PO_FX_FLAG_BLOCKING | PO_FX_FLAG_ASYNC_ONLY)) ==
        (PO_FX_FLAG_BLOCKING | PO_FX_FLAG_ASYNC_ONLY))
    {
        KeBugCheckEx(INTERNAL_POWER_ERROR, POP_FX_BUGCHECK_BAD_REQUEST, (ULONG_PTR)Device, Flags, 1);
    }
    if ((Flags & PO_FX_FLAG_BLOCKING) && KeGetCurrentIrql() >= DISPATCH_LEVEL)
    {
        KeBugCheckEx(INTERNAL_POWER_ERROR, POP_FX_BUGCHECK_BAD_REQUEST, (ULONG_PTR)Device, Flags, 0);
    }
    if (Component >= Device->ComponentCount)
    {
        KeBugCheckEx(INTERNAL_POWER_ERROR, POP_FX_BUGCHECK_BAD_REQUEST, (ULONG_PTR)Device, Component, 2);
    }
}

static
VOID
PopFxCheckComponent(
    _In_ PPOP_FX_DEVICE Device,
    _In_ ULONG Component)
{
    if (Component >= Device->ComponentCount)
    {
        KeBugCheckEx(INTERNAL_POWER_ERROR, POP_FX_BUGCHECK_BAD_REQUEST, (ULONG_PTR)Device, Component, 2);
    }
}

/* Lock held */
static
VOID
PopFxArmDebounceTimer(
    _In_ PPOP_FX_DEVICE Device)
{
    LARGE_INTEGER DueTime;

    if (!Device->DebounceTimerArmed && !Device->Unregistering)
    {
        Device->DebounceTimerArmed = TRUE;
        DueTime.QuadPart = -POP_FX_DEBOUNCE_TIMEOUT;
        KeSetTimer(&Device->DebounceTimer, DueTime, &Device->DebounceDpc);
    }
}

/*
 * The F-state an idle component should be in: the shallowest of the deepest
 * states its latency tolerance allows, the deepest states its residency
 * allows (the expected residency, raised by the time it has been idle) and,
 * when wake is armed, its deepest wakeable state.
 */
static
ULONG
PopFxPreferredIdleState(
    _In_ PPOP_FX_COMPONENT Component)
{
    ULONG i, Latency = 0, Residency = 0, Preferred;

    for (i = 1; i < Component->IdleStateCount; i++)
    {
        if (Component->IdleStates[i].TransitionLatency <= Component->LatencyTolerance)
            Latency = i;
        if (Component->IdleStates[i].ResidencyRequirement <= Component->ExpectedResidency)
            Residency = i;
    }

    Preferred = min(Latency, Residency);
    if (Component->WakeArmed)
        Preferred = min(Preferred, Component->DeepestWakeableIdleState);
    return Preferred;
}

/*
 * Picks the next callback the state of the device asks for and moves the
 * state on to "callback made". Lock held.
 */
static
POP_FX_ACTION
PopFxNextAction(
    _In_ PPOP_FX_DEVICE Device,
    _Out_ PULONG ComponentIndex,
    _Out_ PULONG IdleState)
{
    PPOP_FX_COMPONENT Component;
    BOOLEAN WantPower = FALSE, Transitioning = FALSE;
    ULONGLONG Now = KeQueryInterruptTime();
    ULONG i, Target;

    *ComponentIndex = 0;
    *IdleState = 0;

    if (Device->Unregistering || !Device->Started)
        return PopFxActionNone;

    if (Device->DebounceRelease)
    {
        BOOLEAN StillResident = FALSE;

        Device->DebounceRelease = FALSE;
        for (i = 0; i < Device->ComponentCount; i++)
        {
            Component = &Device->Components[i];
            if (!Component->Resident)
                continue;
            if (Now - Component->LastIdleRequest >= POP_FX_DEBOUNCE_THRESHOLD)
            {
                Component->Resident = FALSE;
                Component->LastIdleRequest = Now;
            }
            else
            {
                StillResident = TRUE;
            }
        }
        if (StillResident)
            PopFxArmDebounceTimer(Device);
    }

    /* The idle state timer: the time idle counts as expected residency */
    if (Device->IdleStateCheck)
    {
        Device->IdleStateCheck = FALSE;
        for (i = 0; i < Device->ComponentCount; i++)
        {
            Component = &Device->Components[i];
            if (Component->Condition == PopFxComponentIdle && Component->ActiveCount == 0 &&
                Now - Component->IdleSince > Component->ExpectedResidency)
            {
                Component->ExpectedResidency = Now - Component->IdleSince;
            }
        }
    }

    for (i = 0; i < Device->ComponentCount; i++)
    {
        Component = &Device->Components[i];

        if (Component->IdleStatePending)
        {
            Transitioning = TRUE;
            continue;
        }

        switch (Component->Condition)
        {
            case PopFxComponentActive:
                /* The last activation reference was dropped */
                if (Component->ActiveCount == 0 && !Component->Resident)
                {
                    Component->Condition = PopFxComponentIdling;
                    *ComponentIndex = i;
                    return PopFxActionIdle;
                }
                break;

            case PopFxComponentIdling:
                break;

            case PopFxComponentIdle:
                if (Component->ActiveCount != 0)
                {
                    /* A component becomes active on a powered device, in F0 */
                    if (Device->DevicePower != PopFxDevicePowerRequired)
                    {
                        WantPower = TRUE;
                        break;
                    }

                    if (Component->IdleState != 0)
                    {
                        Component->IdleStatePending = TRUE;
                        Component->TargetIdleState = 0;
                        *ComponentIndex = i;
                        *IdleState = 0;
                        return PopFxActionIdleState;
                    }

                    Component->Condition = PopFxComponentActive;
                    Device->NotIdleCount++;
                    *ComponentIndex = i;
                    return PopFxActionActive;
                }

                /* F-states change while the device is powered, from one Fx to another through F0 */
                if (Device->DevicePower == PopFxDevicePowerRequired && Component->IdleStateCount > 1)
                {
                    Target = PopFxPreferredIdleState(Component);
                    if (Target != Component->IdleState)
                    {
                        if (Component->IdleState != 0 && Target != 0)
                            Target = 0;
                        Component->IdleStatePending = TRUE;
                        Component->TargetIdleState = Target;
                        *ComponentIndex = i;
                        *IdleState = Target;
                        return PopFxActionIdleState;
                    }
                }
                break;
        }
    }

    if (WantPower)
    {
        /* Not before the driver completed the power-down it was told about */
        if (Device->DevicePower == PopFxDevicePowerNotRequired)
        {
            Device->DevicePower = PopFxDevicePowerRequiring;
            return PopFxActionPowerRequired;
        }
        return PopFxActionNone;
    }

    /* All components idle and settled, device in D0: let it go after the idle timeout */
    if (Device->NotIdleCount == 0 && !Transitioning &&
        Device->DevicePower == PopFxDevicePowerRequired &&
        !(Device->Flags & PO_FX_DEVICE_FLAG_RESERVED_1))
    {
        ULONGLONG Deadline = Device->AllIdleSince + Device->IdleTimeout;

        if (Deadline < Device->AllIdleSince)
            Deadline = MAXULONGLONG;

        if (Now >= Deadline)
        {
            Device->DevicePower = PopFxDevicePowerNotRequiring;
            return PopFxActionPowerNotRequired;
        }

        if (!Device->IdleTimerArmed)
        {
            LARGE_INTEGER DueTime;

            Device->IdleTimerArmed = TRUE;
            DueTime.QuadPart = -(LONGLONG)min(Deadline - Now, (ULONGLONG)MAXLONGLONG);
            KeSetTimer(&Device->IdleTimer, DueTime, &Device->IdleDpc);
        }
    }

    return PopFxActionNone;
}

static
VOID
PopFxCancelIdleTimer(
    _In_ PPOP_FX_DEVICE Device)
{
    /* Lock held; a DPC already queued finds IdleTimerArmed clear and does nothing */
    if (Device->IdleTimerArmed)
    {
        KeCancelTimer(&Device->IdleTimer);
        Device->IdleTimerArmed = FALSE;
    }
}

/* Keeps the idle state timer going while some component may go deeper. Lock held. */
static
VOID
PopFxUpdateIdleStateTimer(
    _In_ PPOP_FX_DEVICE Device)
{
    PPOP_FX_COMPONENT Component;
    BOOLEAN Needed = FALSE;
    LARGE_INTEGER DueTime;
    ULONG i;

    if (!Device->Unregistering && Device->Started)
    {
        for (i = 0; i < Device->ComponentCount; i++)
        {
            Component = &Device->Components[i];
            if (Device->DevicePower == PopFxDevicePowerRequired &&
                Component->Condition == PopFxComponentIdle &&
                Component->ActiveCount == 0 &&
                Component->IdleState + 1 < Component->IdleStateCount)
            {
                Needed = TRUE;
                break;
            }
        }
    }

    if (Needed && !Device->IdleStateTimerArmed)
    {
        Device->IdleStateTimerArmed = TRUE;
        DueTime.QuadPart = -(LONGLONG)POP_FX_IDLE_STATE_PERIOD * 10000;
        KeSetTimerEx(&Device->IdleStateTimer, DueTime, POP_FX_IDLE_STATE_PERIOD, &Device->IdleStateDpc);
    }
    else if (!Needed && Device->IdleStateTimerArmed)
    {
        KeCancelTimer(&Device->IdleStateTimer);
        Device->IdleStateTimerArmed = FALSE;
    }
}

/*
 * Makes every callback the state of the device asks for, one at a time and
 * with no lock held. If another thread is already doing so, it is told to
 * look again instead.
 */
static
VOID
PopFxRunDevice(
    _In_ PPOP_FX_DEVICE Device)
{
    POP_FX_ACTION Action;
    ULONG Index, IdleState;
    KIRQL OldIrql, CallbackIrql;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    if (Device->Running)
    {
        Device->RunAgain = TRUE;
        KeReleaseSpinLock(&Device->Lock, OldIrql);
        return;
    }
    Device->Running = TRUE;
    Device->RunningThread = KeGetCurrentThread();

    for (;;)
    {
        Device->RunAgain = FALSE;
        Action = PopFxNextAction(Device, &Index, &IdleState);
        if (Action == PopFxActionNone)
        {
            if (Device->RunAgain)
                continue;
            break;
        }

        /* The state moved on: blocking activations and unregistering look again */
        KeSetEvent(&Device->StateEvent, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&Device->Lock, OldIrql);

        switch (Action)
        {
            case PopFxActionActive:
                DPRINT("PoFx %p: component %lu active\n", Device, Index);
                if (Device->ComponentActiveConditionCallback)
                    Device->ComponentActiveConditionCallback(Device->DeviceContext, Index);
                break;

            case PopFxActionIdle:
                DPRINT("PoFx %p: component %lu idle\n", Device, Index);
                if (Device->ComponentIdleConditionCallback)
                {
                    KeRaiseIrql(DISPATCH_LEVEL, &CallbackIrql);
                    Device->ComponentIdleConditionCallback(Device->DeviceContext, Index);
                    KeLowerIrql(CallbackIrql);
                }
                else
                {
                    PoFxCompleteIdleCondition((POHANDLE)Device, Index);
                }
                break;

            case PopFxActionIdleState:
                DPRINT("PoFx %p: component %lu to F%lu\n", Device, Index, IdleState);
                Device->ComponentIdleStateCallback(Device->DeviceContext, Index, IdleState);
                break;

            case PopFxActionPowerRequired:
                DPRINT("PoFx %p: device power required\n", Device);
                if (Device->DevicePowerRequiredCallback)
                {
                    KeRaiseIrql(DISPATCH_LEVEL, &CallbackIrql);
                    Device->DevicePowerRequiredCallback(Device->DeviceContext);
                    KeLowerIrql(CallbackIrql);
                }
                else
                {
                    PoFxReportDevicePoweredOn((POHANDLE)Device);
                }
                break;

            case PopFxActionPowerNotRequired:
                DPRINT("PoFx %p: device power not required\n", Device);
                if (Device->DevicePowerNotRequiredCallback)
                {
                    KeRaiseIrql(DISPATCH_LEVEL, &CallbackIrql);
                    Device->DevicePowerNotRequiredCallback(Device->DeviceContext);
                    KeLowerIrql(CallbackIrql);
                }
                else
                {
                    PoFxCompleteDevicePowerNotRequired((POHANDLE)Device);
                }
                break;

            default:
                ASSERT(FALSE);
                break;
        }

        KeAcquireSpinLock(&Device->Lock, &OldIrql);
    }

    PopFxUpdateIdleStateTimer(Device);
    Device->Running = FALSE;
    Device->RunningThread = NULL;
    KeSetEvent(&Device->StateEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&Device->Lock, OldIrql);
}

static WORKER_THREAD_ROUTINE PopFxWorker;

static
VOID
NTAPI
PopFxWorker(
    _In_ PVOID Parameter)
{
    PPOP_FX_DEVICE Device = Parameter;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    Device->WorkQueued = FALSE;
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    PopFxRunDevice(Device);
    PopFxDereferenceDevice(Device);
}

static KDEFERRED_ROUTINE PopFxIdleTimerDpc;

static
VOID
NTAPI
PopFxIdleTimerDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PPOP_FX_DEVICE Device = DeferredContext;
    BOOLEAN Run;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    KeAcquireSpinLockAtDpcLevel(&Device->Lock);
    Run = Device->IdleTimerArmed;
    Device->IdleTimerArmed = FALSE;
    KeReleaseSpinLockFromDpcLevel(&Device->Lock);

    /* The device idle timeout elapsed: device power not required, from here */
    if (Run)
        PopFxRunDevice(Device);
}

static KDEFERRED_ROUTINE PopFxDebounceTimerDpc;

static
VOID
NTAPI
PopFxDebounceTimerDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PPOP_FX_DEVICE Device = DeferredContext;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /* The worker idles the components the debounce kept active */
    KeAcquireSpinLockAtDpcLevel(&Device->Lock);
    if (Device->DebounceTimerArmed)
    {
        Device->DebounceTimerArmed = FALSE;
        Device->DebounceRelease = TRUE;
        PopFxQueueWork(Device);
    }
    KeReleaseSpinLockFromDpcLevel(&Device->Lock);
}

static KDEFERRED_ROUTINE PopFxIdleStateTimerDpc;

static
VOID
NTAPI
PopFxIdleStateTimerDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PPOP_FX_DEVICE Device = DeferredContext;
    BOOLEAN Run;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    KeAcquireSpinLockAtDpcLevel(&Device->Lock);
    Run = Device->IdleStateTimerArmed;
    if (Run)
        Device->IdleStateCheck = TRUE;
    KeReleaseSpinLockFromDpcLevel(&Device->Lock);

    if (Run)
        PopFxRunDevice(Device);
}

/* Reads a V1, V2 or V3 registration */
static
NTSTATUS
PopFxCreateDevice(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ PPO_FX_DEVICE DeviceInfo,
    _Out_ PPOP_FX_DEVICE *NewDevice)
{
    PPO_FX_DEVICE_V1 V1 = (PPO_FX_DEVICE_V1)DeviceInfo;
    PPO_FX_DEVICE_V2 V2 = (PPO_FX_DEVICE_V2)DeviceInfo;
    PPO_FX_DEVICE_V3 V3 = (PPO_FX_DEVICE_V3)DeviceInfo;
    PPO_FX_COMPONENT_ACTIVE_CONDITION_CALLBACK ActiveCallback;
    PPO_FX_COMPONENT_IDLE_CONDITION_CALLBACK IdleCallback;
    PPO_FX_COMPONENT_IDLE_STATE_CALLBACK IdleStateCallback;
    PPO_FX_COMPONENT_IDLE_STATE IdleStates;
    PPOP_FX_DEVICE Device;
    PPOP_FX_COMPONENT Component;
    ULONG ComponentCount, i, j, IdleStateTotal = 0;
    SIZE_T Size;

    switch (DeviceInfo->Version)
    {
        case PO_FX_VERSION_V1:
            ComponentCount = V1->ComponentCount;
            ActiveCallback = V1->ComponentActiveConditionCallback;
            IdleCallback = V1->ComponentIdleConditionCallback;
            IdleStateCallback = V1->ComponentIdleStateCallback;
            break;

        case PO_FX_VERSION_V2:
            ComponentCount = V2->ComponentCount;
            ActiveCallback = V2->ComponentActiveConditionCallback;
            IdleCallback = V2->ComponentIdleConditionCallback;
            IdleStateCallback = V2->ComponentIdleStateCallback;
            break;

        case PO_FX_VERSION_V3:
            /* Directed power transitions need both directions */
            if ((V3->DirectedPowerUpCallback == NULL) != (V3->DirectedPowerDownCallback == NULL))
                return STATUS_INVALID_PARAMETER;
            ComponentCount = V3->ComponentCount;
            ActiveCallback = V3->ComponentActiveConditionCallback;
            IdleCallback = V3->ComponentIdleConditionCallback;
            IdleStateCallback = V3->ComponentIdleStateCallback;
            break;

        default:
            return STATUS_INVALID_PARAMETER;
    }

    if (ComponentCount == 0)
        return STATUS_INVALID_PARAMETER;

    for (i = 0; i < ComponentCount; i++)
    {
        ULONG IdleStateCount, DeepestWakeable;

        if (DeviceInfo->Version == PO_FX_VERSION_V1)
        {
            IdleStateCount = V1->Components[i].IdleStateCount;
            DeepestWakeable = V1->Components[i].DeepestWakeableIdleState;
        }
        else
        {
            PPO_FX_COMPONENT_V2 C2 = (DeviceInfo->Version == PO_FX_VERSION_V2) ?
                                     &V2->Components[i] : &V3->Components[i];

            IdleStateCount = C2->IdleStateCount;
            DeepestWakeable = C2->DeepestWakeableIdleState;

            /* Components this one depends on */
            for (j = 0; j < C2->ProviderCount; j++)
            {
                if (C2->Providers[j] >= ComponentCount)
                    return STATUS_INVALID_PARAMETER;
            }
        }

        if (IdleStateCount == 0)
            return STATUS_INVALID_PARAMETER;

        /* Components with F-states to go to are told about them */
        if (IdleStateCount > 1 &&
            (ActiveCallback == NULL || IdleCallback == NULL || IdleStateCallback == NULL ||
             DeepestWakeable >= IdleStateCount))
        {
            return STATUS_INVALID_PARAMETER;
        }

        if (IdleStateTotal + IdleStateCount < IdleStateTotal)
            return STATUS_INVALID_PARAMETER;
        IdleStateTotal += IdleStateCount;
    }

    if (ComponentCount > (MAXULONG - sizeof(POP_FX_DEVICE)) / sizeof(POP_FX_COMPONENT) ||
        IdleStateTotal > (MAXULONG / 2) / sizeof(PO_FX_COMPONENT_IDLE_STATE))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Size = FIELD_OFFSET(POP_FX_DEVICE, Components[ComponentCount]);
    Size = ALIGN_UP_BY(Size, sizeof(ULONGLONG));
    Device = ExAllocatePoolZero(NonPagedPool,
                                Size + IdleStateTotal * sizeof(PO_FX_COMPONENT_IDLE_STATE),
                                TAG_PO_FX);
    if (Device == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    IdleStates = (PPO_FX_COMPONENT_IDLE_STATE)((PUCHAR)Device + Size);

    Device->Signature = POP_FX_SIGNATURE;
    Device->Version = DeviceInfo->Version;
    Device->ComponentCount = ComponentCount;
    Device->ComponentActiveConditionCallback = ActiveCallback;
    Device->ComponentIdleConditionCallback = IdleCallback;
    Device->ComponentIdleStateCallback = IdleStateCallback;

    switch (DeviceInfo->Version)
    {
        case PO_FX_VERSION_V1:
            Device->DevicePowerRequiredCallback = V1->DevicePowerRequiredCallback;
            Device->DevicePowerNotRequiredCallback = V1->DevicePowerNotRequiredCallback;
            Device->PowerControlCallback = V1->PowerControlCallback;
            Device->DeviceContext = V1->DeviceContext;
            break;

        case PO_FX_VERSION_V2:
            Device->Flags = V2->Flags;
            Device->DevicePowerRequiredCallback = V2->DevicePowerRequiredCallback;
            Device->DevicePowerNotRequiredCallback = V2->DevicePowerNotRequiredCallback;
            Device->PowerControlCallback = V2->PowerControlCallback;
            Device->DeviceContext = V2->DeviceContext;
            break;

        case PO_FX_VERSION_V3:
            Device->Flags = V3->Flags;
            Device->DevicePowerRequiredCallback = V3->DevicePowerRequiredCallback;
            Device->DevicePowerNotRequiredCallback = V3->DevicePowerNotRequiredCallback;
            Device->PowerControlCallback = V3->PowerControlCallback;
            /* Directed (modern standby) transitions are never started here */
            Device->DirectedPowerUpCallback = V3->DirectedPowerUpCallback;
            Device->DirectedPowerDownCallback = V3->DirectedPowerDownCallback;
            Device->DeviceContext = V3->DeviceContext;
            break;
    }

    for (i = 0; i < ComponentCount; i++)
    {
        Component = &Device->Components[i];

        if (DeviceInfo->Version == PO_FX_VERSION_V1)
        {
            Component->Id = V1->Components[i].Id;
            Component->IdleStateCount = V1->Components[i].IdleStateCount;
            Component->DeepestWakeableIdleState = V1->Components[i].DeepestWakeableIdleState;
            RtlCopyMemory(IdleStates, V1->Components[i].IdleStates,
                          Component->IdleStateCount * sizeof(*IdleStates));
        }
        else
        {
            PPO_FX_COMPONENT_V2 C2 = (DeviceInfo->Version == PO_FX_VERSION_V2) ?
                                     &V2->Components[i] : &V3->Components[i];

            Component->Id = C2->Id;
            Component->Flags = C2->Flags;
            Component->IdleStateCount = C2->IdleStateCount;
            Component->DeepestWakeableIdleState = C2->DeepestWakeableIdleState;
            RtlCopyMemory(IdleStates, C2->IdleStates,
                          Component->IdleStateCount * sizeof(*IdleStates));
        }

        Component->IdleStates = IdleStates;
        IdleStates += Component->IdleStateCount;

        /* Registered components are active, in F0, with one activation reference */
        Component->ActiveCount = 1;
        Component->Condition = PopFxComponentActive;
        Component->LatencyTolerance = MAXULONGLONG;
    }

    KeInitializeSpinLock(&Device->Lock);
    Device->RefCount = 1;
    Device->DevicePower = PopFxDevicePowerRequired;
    Device->NotIdleCount = ComponentCount;
    KeInitializeTimer(&Device->IdleTimer);
    KeInitializeDpc(&Device->IdleDpc, PopFxIdleTimerDpc, Device);
    KeInitializeTimer(&Device->DebounceTimer);
    KeInitializeDpc(&Device->DebounceDpc, PopFxDebounceTimerDpc, Device);
    KeInitializeTimer(&Device->IdleStateTimer);
    KeInitializeDpc(&Device->IdleStateDpc, PopFxIdleStateTimerDpc, Device);
    ExInitializeWorkItem(&Device->WorkItem, PopFxWorker, Device);
    KeInitializeEvent(&Device->StateEvent, NotificationEvent, FALSE);

    ObReferenceObject(Pdo);
    Device->Pdo = Pdo;

    *NewDevice = Device;
    return STATUS_SUCCESS;
}

/* Drops one activation reference. Lock held. */
static
VOID
PopFxReleaseComponent(
    _In_ PPOP_FX_DEVICE Device,
    _In_ ULONG Index,
    _In_ ULONG Flags)
{
    PPOP_FX_COMPONENT Component = &Device->Components[Index];
    ULONGLONG Now;

    if (Component->ActiveCount == 0)
        KeBugCheckEx(INTERNAL_POWER_ERROR, POP_FX_BUGCHECK_IDLE_UNDERFLOW, (ULONG_PTR)Device, Index, 0);

    if (--Component->ActiveCount != 0)
        return;

    /* Idled again right after the last time: keep it active a while longer */
    Now = KeQueryInterruptTime();
    if (!(Component->Flags & PO_FX_COMPONENT_FLAG_NO_DEBOUNCE) &&
        !(Flags & PO_FX_FLAG_BLOCKING) &&
        Component->LastIdleRequest != 0 &&
        Now - Component->LastIdleRequest < POP_FX_DEBOUNCE_THRESHOLD &&
        Component->Condition == PopFxComponentActive)
    {
        Component->Resident = TRUE;
        PopFxArmDebounceTimer(Device);
        return;
    }
    Component->LastIdleRequest = Now;
}

/* Takes an activation reference on every component and waits until all are active */
static
VOID
PopFxActivateAllAndWait(
    _In_ PPOP_FX_DEVICE Device,
    _In_ BOOLEAN ForSleep)
{
    PPOP_FX_COMPONENT Component;
    BOOLEAN Active;
    KIRQL OldIrql;
    ULONG i;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    ASSERT(Device->RunningThread != KeGetCurrentThread());
    if (ForSleep)
        Device->SxActivations++;
    for (i = 0; i < Device->ComponentCount; i++)
    {
        Device->Components[i].ActiveCount++;
        Device->Components[i].Resident = FALSE;
    }
    PopFxCancelIdleTimer(Device);
    PopFxQueueWork(Device);

    for (;;)
    {
        Active = TRUE;
        for (i = 0; i < Device->ComponentCount; i++)
        {
            Component = &Device->Components[i];
            if (Component->Condition != PopFxComponentActive || Component->IdleStatePending)
                Active = FALSE;
        }
        if (Active || !Device->Started || Device->Unregistering)
            break;

        KeClearEvent(&Device->StateEvent);
        KeReleaseSpinLock(&Device->Lock, OldIrql);
        KeWaitForSingleObject(&Device->StateEvent, Executive, KernelMode, FALSE, NULL);
        KeAcquireSpinLock(&Device->Lock, &OldIrql);
    }
    KeReleaseSpinLock(&Device->Lock, OldIrql);
}

/* The registration of a PDO, referenced, or NULL */
static
PPOP_FX_DEVICE
PopFxReferenceDeviceByPdo(
    _In_ PDEVICE_OBJECT Pdo)
{
    PPOP_FX_DEVICE Device, Found = NULL;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&PopFxDeviceListLock, &OldIrql);
    for (Entry = PopFxDeviceList.Flink; Entry != &PopFxDeviceList; Entry = Entry->Flink)
    {
        Device = CONTAINING_RECORD(Entry, POP_FX_DEVICE, Link);
        if (Device->Pdo == Pdo)
        {
            InterlockedIncrement(&Device->RefCount);
            Found = Device;
            break;
        }
    }
    KeReleaseSpinLock(&PopFxDeviceListLock, OldIrql);
    return Found;
}

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
PopFxInitialize(VOID)
{
    KeInitializeSpinLock(&PopFxDeviceListLock);
    InitializeListHead(&PopFxDeviceList);
}

/*
 * Before a sleep or hibernate IRP goes to a device, its components are made
 * active (the device powered, every component in F0), so that it sees the
 * system transition in D0; PoFxReportDevicePoweredOn after the resume gives
 * these activations back.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
PopFxActivateDeviceForSystemTransition(
    _In_ PDEVICE_OBJECT Pdo)
{
    PPOP_FX_DEVICE Device;

    PAGED_CODE();

    Device = PopFxReferenceDeviceByPdo(Pdo);
    if (Device == NULL)
        return;

    PopFxActivateAllAndWait(Device, TRUE);
    PopFxDereferenceDevice(Device);
}

/* The sleep IRP sent after PopFxActivateDeviceForSystemTransition completed */
VOID
NTAPI
PopFxIncrementDeviceSleepCount(
    _In_ PDEVICE_OBJECT Pdo)
{
    PPOP_FX_DEVICE Device;
    KIRQL OldIrql;

    Device = PopFxReferenceDeviceByPdo(Pdo);
    if (Device == NULL)
        return;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    Device->SleepCount++;
    KeReleaseSpinLock(&Device->Lock, OldIrql);
    PopFxDereferenceDevice(Device);
}

/* PUBLIC FUNCTIONS **********************************************************/

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
PoFxRegisterDevice(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ PPO_FX_DEVICE Device,
    _Out_ POHANDLE *Handle)
{
    PPOP_FX_DEVICE FxDevice, Existing;
    PLIST_ENTRY Entry;
    NTSTATUS Status;
    KIRQL OldIrql;

    PAGED_CODE();

    *Handle = NULL;

    if (Pdo == NULL || IopGetDeviceNode(Pdo) == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = PopFxCreateDevice(Pdo, Device, &FxDevice);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PoFxRegisterDevice(%p): version %lu refused, 0x%lx\n", Pdo, Device->Version, Status);
        return Status;
    }

    KeAcquireSpinLock(&PopFxDeviceListLock, &OldIrql);
    for (Entry = PopFxDeviceList.Flink; Entry != &PopFxDeviceList; Entry = Entry->Flink)
    {
        Existing = CONTAINING_RECORD(Entry, POP_FX_DEVICE, Link);

        /* A device registers once */
        if (Existing->Pdo == Pdo)
        {
            KeBugCheckEx(INTERNAL_POWER_ERROR, POP_FX_BUGCHECK_ALREADY_REGISTERED,
                         (ULONG_PTR)Pdo, (ULONG_PTR)Existing, 0);
        }
    }
    InsertTailList(&PopFxDeviceList, &FxDevice->Link);
    KeReleaseSpinLock(&PopFxDeviceListLock, OldIrql);

    DPRINT("PoFxRegisterDevice(%p): %p, %lu components\n", Pdo, FxDevice, FxDevice->ComponentCount);
    *Handle = (POHANDLE)FxDevice;
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
PoFxStartDevicePowerManagement(
    _In_ POHANDLE Handle)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);
    ULONG i;
    KIRQL OldIrql;

    PAGED_CODE();

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    ASSERT(!Device->Started);
    Device->Started = TRUE;

    /* Drop the reference each component was registered with */
    for (i = 0; i < Device->ComponentCount; i++)
        PopFxReleaseComponent(Device, i, PO_FX_FLAG_BLOCKING);
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    PopFxRunDevice(Device);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
PoFxUnregisterDevice(
    _In_ POHANDLE Handle)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);
    KIRQL OldIrql;

    PAGED_CODE();

    /* Every component active, the device powered: the driver still answers callbacks */
    PopFxActivateAllAndWait(Device, FALSE);

    KeAcquireSpinLock(&Device->Lock, &OldIrql);

    /* No callback from now on; wait for the one in progress */
    Device->Unregistering = TRUE;
    KeCancelTimer(&Device->IdleTimer);
    KeCancelTimer(&Device->DebounceTimer);
    KeCancelTimer(&Device->IdleStateTimer);
    Device->IdleTimerArmed = Device->DebounceTimerArmed = Device->IdleStateTimerArmed = FALSE;
    while (Device->Running)
    {
        KeClearEvent(&Device->StateEvent);
        KeReleaseSpinLock(&Device->Lock, OldIrql);
        KeWaitForSingleObject(&Device->StateEvent, Executive, KernelMode, FALSE, NULL);
        KeAcquireSpinLock(&Device->Lock, &OldIrql);
    }
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    /* A timer DPC that already started finds the timers disarmed */
    KeFlushQueuedDpcs();

    KeAcquireSpinLock(&PopFxDeviceListLock, &OldIrql);
    RemoveEntryList(&Device->Link);
    KeReleaseSpinLock(&PopFxDeviceListLock, OldIrql);

    DPRINT("PoFxUnregisterDevice(%p)\n", Device);

    /* A queued worker holds its own reference */
    PopFxDereferenceDevice(Device);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
PoFxActivateComponent(
    _In_ POHANDLE Handle,
    _In_ ULONG Component,
    _In_ ULONG Flags)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);
    PPOP_FX_COMPONENT FxComponent;
    BOOLEAN Run = FALSE;
    KIRQL OldIrql;

    PopFxCheckRequest(Device, Component, Flags);
    FxComponent = &Device->Components[Component];

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    if (FxComponent->ActiveCount++ == 0)
    {
        /* Debounced: it never stopped being active */
        FxComponent->Resident = FALSE;

        /* The device idle timeout no longer runs; power is still required */
        PopFxCancelIdleTimer(Device);

        if (Flags & PO_FX_FLAG_ASYNC_ONLY)
            PopFxQueueWork(Device);
        else
            Run = TRUE;
    }
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    if (Run)
        PopFxRunDevice(Device);

    if (Flags & PO_FX_FLAG_BLOCKING)
    {
        /* Until the component is active */
        KeAcquireSpinLock(&Device->Lock, &OldIrql);
        while (Device->Started &&
               (FxComponent->Condition != PopFxComponentActive || FxComponent->IdleStatePending))
        {
            KeClearEvent(&Device->StateEvent);
            KeReleaseSpinLock(&Device->Lock, OldIrql);
            KeWaitForSingleObject(&Device->StateEvent, Executive, KernelMode, FALSE, NULL);
            KeAcquireSpinLock(&Device->Lock, &OldIrql);
        }
        KeReleaseSpinLock(&Device->Lock, OldIrql);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
PoFxIdleComponent(
    _In_ POHANDLE Handle,
    _In_ ULONG Component,
    _In_ ULONG Flags)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);
    BOOLEAN Run = FALSE;
    KIRQL OldIrql;

    PopFxCheckRequest(Device, Component, Flags);

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    PopFxReleaseComponent(Device, Component, Flags);
    if (Device->Components[Component].ActiveCount == 0)
    {
        if (Flags & PO_FX_FLAG_ASYNC_ONLY)
            PopFxQueueWork(Device);
        else
            Run = TRUE;
    }
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    if (Run)
        PopFxRunDevice(Device);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
PoFxCompleteIdleCondition(
    _In_ POHANDLE Handle,
    _In_ ULONG Component)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);
    PPOP_FX_COMPONENT FxComponent;
    KIRQL OldIrql;

    PopFxCheckComponent(Device, Component);
    FxComponent = &Device->Components[Component];

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    if (FxComponent->Condition != PopFxComponentIdling)
    {
        KeBugCheckEx(INTERNAL_POWER_ERROR, POP_FX_BUGCHECK_COMPLETE_UNDERFLOW,
                     (ULONG_PTR)Device, Component, 2);
    }
    FxComponent->Condition = PopFxComponentIdle;
    FxComponent->IdleSince = KeQueryInterruptTime();
    ASSERT(Device->NotIdleCount != 0);
    if (--Device->NotIdleCount == 0)
        Device->AllIdleSince = FxComponent->IdleSince;
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    PopFxRunDevice(Device);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
PoFxCompleteIdleState(
    _In_ POHANDLE Handle,
    _In_ ULONG Component)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);
    PPOP_FX_COMPONENT FxComponent;
    KIRQL OldIrql;

    PopFxCheckComponent(Device, Component);
    FxComponent = &Device->Components[Component];

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    ASSERT(FxComponent->IdleStatePending);
    if (FxComponent->IdleStatePending)
    {
        FxComponent->IdleStatePending = FALSE;
        FxComponent->IdleState = FxComponent->TargetIdleState;
    }
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    PopFxRunDevice(Device);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
PoFxCompleteDevicePowerNotRequired(
    _In_ POHANDLE Handle)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);
    KIRQL OldIrql;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    if (Device->DevicePower != PopFxDevicePowerNotRequiring)
    {
        KeBugCheckEx(INTERNAL_POWER_ERROR, POP_FX_BUGCHECK_COMPLETE_UNDERFLOW,
                     (ULONG_PTR)Device, Device->DevicePower, 0);
    }
    Device->DevicePower = PopFxDevicePowerNotRequired;
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    /* A component activated meanwhile needs the device powered again */
    PopFxRunDevice(Device);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
PoFxReportDevicePoweredOn(
    _In_ POHANDLE Handle)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);
    KIRQL OldIrql;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);

    /* Back from system sleep: give the activations taken for it back */
    if (Device->SleepCount != 0)
    {
        ULONG Count = Device->SleepCount, i, j;

        if (Count > Device->SxActivations)
        {
            KeBugCheckEx(INTERNAL_POWER_ERROR, POP_FX_BUGCHECK_SX_COUNT_MISMATCH,
                         (ULONG_PTR)Device, Count, Device->SxActivations);
        }
        Device->SleepCount = 0;
        Device->SxActivations -= Count;
        for (i = 0; i < Device->ComponentCount; i++)
        {
            for (j = 0; j < Count; j++)
                PopFxReleaseComponent(Device, i, 0);
        }
        PopFxQueueWork(Device);
        KeReleaseSpinLock(&Device->Lock, OldIrql);
        return;
    }

    if (Device->DevicePower != PopFxDevicePowerRequiring)
    {
        KeBugCheckEx(INTERNAL_POWER_ERROR, POP_FX_BUGCHECK_COMPLETE_UNDERFLOW,
                     (ULONG_PTR)Device, Device->DevicePower, 1);
    }
    Device->DevicePower = PopFxDevicePowerRequired;

    /* The components waiting for power become active from the worker */
    PopFxQueueWork(Device);
    KeReleaseSpinLock(&Device->Lock, OldIrql);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
PoFxSetDeviceIdleTimeout(
    _In_ POHANDLE Handle,
    _In_ ULONGLONG IdleTimeout)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);
    KIRQL OldIrql;

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    Device->IdleTimeout = min(IdleTimeout, 0x3FFFFFFFFFFFFFFFULL);
    KeReleaseSpinLock(&Device->Lock, OldIrql);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
PoFxSetComponentLatency(
    _In_ POHANDLE Handle,
    _In_ ULONG Component,
    _In_ ULONGLONG Latency)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);
    KIRQL OldIrql;

    PopFxCheckComponent(Device, Component);

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    Device->Components[Component].LatencyTolerance = Latency;
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    PopFxRunDevice(Device);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
PoFxSetComponentResidency(
    _In_ POHANDLE Handle,
    _In_ ULONG Component,
    _In_ ULONGLONG Residency)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);
    KIRQL OldIrql;

    PopFxCheckComponent(Device, Component);

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    Device->Components[Component].ExpectedResidency = Residency;
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    PopFxRunDevice(Device);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
PoFxSetComponentWake(
    _In_ POHANDLE Handle,
    _In_ ULONG Component,
    _In_ BOOLEAN WakeHint)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);
    KIRQL OldIrql;

    PopFxCheckComponent(Device, Component);

    KeAcquireSpinLock(&Device->Lock, &OldIrql);
    Device->Components[Component].WakeArmed = WakeHint;
    KeReleaseSpinLock(&Device->Lock, OldIrql);

    PopFxRunDevice(Device);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
NTAPI
PoFxPowerControl(
    _In_ POHANDLE Handle,
    _In_ LPCGUID PowerControlCode,
    _In_reads_bytes_opt_(InBufferSize) PVOID InBuffer,
    _In_ SIZE_T InBufferSize,
    _Out_writes_bytes_opt_(OutBufferSize) PVOID OutBuffer,
    _In_ SIZE_T OutBufferSize,
    _Out_opt_ PSIZE_T BytesReturned)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PowerControlCode);
    UNREFERENCED_PARAMETER(InBuffer);
    UNREFERENCED_PARAMETER(InBufferSize);
    UNREFERENCED_PARAMETER(OutBuffer);
    UNREFERENCED_PARAMETER(OutBufferSize);

    /* Power control requests go to the device's PEP, and there is none */
    if (BytesReturned)
        *BytesReturned = 0;
    return STATUS_NOT_SUPPORTED;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
PoFxNotifySurprisePowerOn(
    _In_ PDEVICE_OBJECT Pdo)
{
    /* Only a PEP cares that a device was powered without asking */
    UNREFERENCED_PARAMETER(Pdo);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
PoFxCompleteDirectedPowerDown(
    _In_ POHANDLE Handle)
{
    PPOP_FX_DEVICE Device = PopFxGetDevice(Handle);

    /* Directed power-down is a modern standby transition, never started here */
    KeBugCheckEx(INTERNAL_POWER_ERROR, POP_FX_BUGCHECK_COMPLETE_UNDERFLOW, (ULONG_PTR)Device, 0, 3);
}
