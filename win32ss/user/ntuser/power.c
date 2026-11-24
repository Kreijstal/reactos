/*
 * PROJECT:         ReactOS Win32k subsystem
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         Power management of the Win32 kernel-mode subsystem
 * COPYRIGHT:       Copyright 2024 George Bișoc <george.bisoc@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <win32k.h>
DBG_DEFAULT_CHANNEL(UserPowerManager);

/* GLOBALS *******************************************************************/

LIST_ENTRY gPowerCalloutsQueueList;
PFAST_MUTEX gpPowerCalloutMutexLock;
PKEVENT gpPowerRequestCalloutEvent;
PKTHREAD gpPowerCalloutMutexOwnerThread;

#if (WINVER >= 0x0600)
/*
 * A window registered by RegisterPowerSettingNotification. Each one holds a
 * kernel power setting callback; the kernel calls it once with the current
 * value right after registration and again on each change, on its power
 * setting worker thread. The value goes to the window as WM_POWERBROADCAST
 * with PBT_POWERSETTINGCHANGE through the power callout queue.
 */
typedef struct _USER_POWER_SETTING_NOTIFY
{
    LIST_ENTRY Link;
    PPROCESSINFO ppi;
    HWND hwnd;
    /* What the window asked for, reported in the POWERBROADCAST_SETTING */
    GUID Guid;
    PVOID PoHandle;
    /* The HPOWERNOTIFY of the registration */
    ULONG_PTR Notify;
} USER_POWER_SETTING_NOTIFY, *PUSER_POWER_SETTING_NOTIFY;

static LIST_ENTRY gPowerSettingNotifyList;
static PFAST_MUTEX gpPowerSettingNotifyLock;
static ULONG_PTR gLastPowerSettingNotify;

/* A session setting the kernel does not produce: the session display is the console display */
static const GUID GUID_SESSION_DISPLAY_STATUS_ =
    { 0x2b84c20e, 0xad23, 0x4ddf, { 0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5 } };
static const GUID GUID_CONSOLE_DISPLAY_STATE_ =
    { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };
#endif

/* PRIVATE FUNCTIONS *********************************************************/

/**
 * @brief
 * Handles a power event as a result from an incoming power
 * callout from the kernel power manager.
 *
 * @param[in] pParameters
 * A pointer to the power event parameters containing the
 * power event type and sub-code serving as additional datum
 * for that power event type.
 */
static
VOID
IntHandlePowerEventWorker(
    _In_ PWIN32_POWEREVENT_PARAMETERS pParameters)
{
    PSPOWEREVENTTYPE PwrEventType;
    ULONG Code;

    /* Cache the power event parameters and handle the power callout */
    PwrEventType = pParameters->EventNumber;
    Code = pParameters->Code;
    switch (PwrEventType)
    {
        case PsW32SystemTime:
        {
            /*
             * The power manager of the kernel notified us of an impending
             * time change, broadcast this notification to all present windows.
             */
            UserSendNotifyMessage(HWND_BROADCAST,
                                  WM_TIMECHANGE,
                                  0,
                                  0);
            break;
        }

        default:
        {
            TRACE("Power event of type %d is currently UNIMPLEMENTED (code %lu)\n", PwrEventType, Code);
            break;
        }
    }
}

#if (WINVER >= 0x0600)
/* Tells a registered window the new value of a power setting */
static
VOID
IntHandlePowerSettingWorker(
    _In_ PWIN32POWERCALLOUT pPowerCallout)
{
    /* The receiver gets its own copy of the POWERBROADCAST_SETTING (see MsgMemorySize) */
    co_IntSendMessageNoWait(pPowerCallout->hwndRecipient,
                            WM_POWERBROADCAST,
                            PBT_POWERSETTINGCHANGE,
                            (LPARAM)&pPowerCallout->Setting);
}
#endif

/**
 * @brief
 * Validates the power event parameters that come from
 * a power callout from the kernel power manager.
 *
 * @param[in] pParameters
 * A pointer to the power event parameters containing the
 * power event type of which is to be validated against
 * valid power events.
 *
 * @return
 * Returns STATUS_INVALID_PARAMETER if the captured power
 * event type is unknown, otherwise STATUS_SUCCESS is returned.
 */
static
NTSTATUS
IntValidateWin32PowerParams(
    _In_ PWIN32_POWEREVENT_PARAMETERS pParameters)
{
    PSPOWEREVENTTYPE PwrEventType;

    /* Capture the event number and check if it is within bounds */
    PwrEventType = pParameters->EventNumber;
    if (PwrEventType < PsW32FullWake || PwrEventType > PsW32MonitorOff)
    {
        TRACE("Unknown event number found -> %d\n", PwrEventType);
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Gets the oldest pending power callout from the global queue
 * list and returns it to the caller. Note that the returned
 * power callout is delisted from the list.
 *
 * @return
 * Returns a pointer to a power callout to the caller. If the list is
 * empty then it will return NULL.
 *
 * @remarks
 * The caller ASSUMES responsibility to lock down the power callout list
 * before it begins enumerating the global list!
 */
static
PWIN32POWERCALLOUT
IntGetNextPowerCallout(VOID)
{
    PLIST_ENTRY Entry;

    /* Ensure the current calling thread owns the power callout lock */
    ASSERT_POWER_CALLOUT_LOCK_ACQUIRED();

    /* This list is empty, acknowledge the caller */
    if (IsListEmpty(&gPowerCalloutsQueueList))
    {
        return NULL;
    }

    /* Delist the oldest power callout and give it to the caller */
    Entry = RemoveHeadList(&gPowerCalloutsQueueList);
    return CONTAINING_RECORD(Entry, WIN32POWERCALLOUT, Link);
}

/**
 * @brief
 * Deploys all pending power callouts to appropriate power
 * callout workers.
 */
static
VOID
IntDeployPowerCallout(VOID)
{
    PWIN32POWERCALLOUT pWin32PwrCallout;

    /* The caller locked the entire USER subsystem down as we do particular stuff */
    ASSERT(UserIsEnteredExclusive());

    /*
     * FIXME: While we did indeed lock the USER subsystem, there is a risk
     * of the current calling thread might die or hang, so we should probably
     * lock the thread while we deploy our power callout. The thread info
     * provides a thread lock field for this purpose (see the ptl member from
     * the _THREADINFO structure) but ReactOS lacks implementation to handle
     * this. Suppose a thread happens to get into this fate, the power callout
     * would never get signaled...
     */

    /* Deploy all the pending power callouts to the appropriate callout workers */
    IntAcquirePowerCalloutLock();
    while ((pWin32PwrCallout = IntGetNextPowerCallout()) != NULL)
    {
        if (pWin32PwrCallout->Type == POWER_CALLOUT_EVENT)
        {
            IntHandlePowerEventWorker(&pWin32PwrCallout->Params);
        }
#if (WINVER >= 0x0600)
        else if (pWin32PwrCallout->Type == POWER_CALLOUT_SETTING)
        {
            IntHandlePowerSettingWorker(pWin32PwrCallout);
        }
#endif
        else // POWER_CALLOUT_STATE
        {
            ERR("Power state callout management is currently not implemented!\n");
        }

        /* We are done with this power callout */
        ExFreePoolWithTag(pWin32PwrCallout, USERTAG_POWER);
    }

    /* Release what we locked */
    IntReleasePowerCalloutLock();
}

/**
 * @brief
 * Enlists a newly allocated power callout into the queue list
 * for later processing.
 *
 * @param[in] pPowerCallout
 * A pointer to a power callout that is to be inserted into the
 * queue list.
 */
static
VOID
IntEnlistPowerCallout(
    _In_ PWIN32POWERCALLOUT pPowerCallout)
{
    PETHREAD CurrentThread;

    /* Enlist it to the queue already */
    IntAcquirePowerCalloutLock();
    InsertTailList(&gPowerCalloutsQueueList, &pPowerCallout->Link);
    IntReleasePowerCalloutLock();

    /*
     * We have to let CSRSS process this power callout if one of the
     * following conditions is TRUE for the current calling thread:
     *
     * - The process of the calling thread is a system process;
     * - The process of the calling thread is attached;
     * - The current calling thread is not a Win32 thread.
     *
     * For the second point, we cannot process the power callout ourselves
     * as we must lock down USER exclusively for our own purpose, which requires
     * us to be in a critical section. So we do not want to fiddle with a process
     * that is attached with others.
     */
    CurrentThread = PsGetCurrentThread();
    if (PsIsSystemThread(CurrentThread) ||
        KeIsAttachedProcess() ||
        !IntIsThreadWin32Thread(CurrentThread))
    {
        /* Alert CSRSS of the presence of an enqueued power callout */
        KeSetEvent(gpPowerRequestCalloutEvent, EVENT_INCREMENT, FALSE);
        return;
    }

    /* Handle this power callout ourselves */
    UserEnterExclusive();
    IntDeployPowerCallout();
    UserLeave();
}

#if (WINVER >= 0x0600)
/* Called by the kernel power manager on its power setting worker thread */
static POWER_SETTING_CALLBACK IntPowerSettingCallback;

static
NTSTATUS
NTAPI
IntPowerSettingCallback(
    _In_ LPCGUID SettingGuid,
    _In_reads_bytes_(ValueLength) PVOID Value,
    _In_ ULONG ValueLength,
    _Inout_opt_ PVOID Context)
{
    PUSER_POWER_SETTING_NOTIFY pNotify = Context;
    PWIN32POWERCALLOUT pPowerCallout;

    UNREFERENCED_PARAMETER(SettingGuid);

    /* The registration is not freed while its callback runs (PoUnregisterPowerSettingCallback waits) */
    pPowerCallout = ExAllocatePoolZero(NonPagedPool,
                                       FIELD_OFFSET(WIN32POWERCALLOUT, Setting.Data) + ValueLength,
                                       USERTAG_POWER);
    if (pPowerCallout == NULL)
    {
        ERR("No memory to report a power setting change to window %p\n", pNotify->hwnd);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pPowerCallout->Type = POWER_CALLOUT_SETTING;
    pPowerCallout->hwndRecipient = pNotify->hwnd;
    pPowerCallout->Setting.PowerSetting = pNotify->Guid;
    pPowerCallout->Setting.DataLength = ValueLength;
    RtlCopyMemory(pPowerCallout->Setting.Data, Value, ValueLength);

    IntEnlistPowerCallout(pPowerCallout);
    return STATUS_SUCCESS;
}
#endif

/* PUBLIC FUNCTIONS **********************************************************/

/**
 * @brief
 * Initializes the power management side of Win32 kernel-mode
 * subsystem component. This enables communication between
 * the power manager of the NT kernel and Win32k.
 *
 * @param[in] hPowerRequestEvent
 * A handle to the global power request event, provided by the
 * Winsrv module. This allows CSRSS to be notified of power callouts
 * that cannot be handled by Win32k.
 *
 * @return
 * Returns STATUS_INSUFFICIENT_RESOURCES if pool allocation for the
 * power callout lock has failed due to lack of necessary memory.
 * A failure NTSTATUS code is returned if the power request event
 * could not be referenced. Otherwise STATUS_SUCCESS is returned to
 * indicate the power management has initialized successfully.
 */
NTSTATUS
NTAPI
IntInitWin32PowerManagement(
    _In_ HANDLE hPowerRequestEvent)
{
    NTSTATUS Status;

    /* Allocate memory pool for the power callout mutex */
    gpPowerCalloutMutexLock = ExAllocatePoolZero(NonPagedPool,
                                                 sizeof(FAST_MUTEX),
                                                 USERTAG_POWER);
    if (gpPowerCalloutMutexLock == NULL)
    {
        ERR("Failed to allocate pool of memory for the power callout mutex\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Initialize the mutex and owner thread */
    ExInitializeFastMutex(gpPowerCalloutMutexLock);
    gpPowerCalloutMutexOwnerThread = NULL;

#if (WINVER >= 0x0600)
    gpPowerSettingNotifyLock = ExAllocatePoolZero(NonPagedPool,
                                                  sizeof(FAST_MUTEX),
                                                  USERTAG_POWER);
    if (gpPowerSettingNotifyLock == NULL)
    {
        ERR("Failed to allocate pool of memory for the power setting notification mutex\n");
        ExFreePoolWithTag(gpPowerCalloutMutexLock, USERTAG_POWER);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ExInitializeFastMutex(gpPowerSettingNotifyLock);
    InitializeListHead(&gPowerSettingNotifyList);
#endif

    /* Initialize the global queue list and the power callout (aka request) event object */
    InitializeListHead(&gPowerCalloutsQueueList);
    Status = ObReferenceObjectByHandle(hPowerRequestEvent,
                                       EVENT_ALL_ACCESS,
                                       *ExEventObjectType,
                                       KernelMode,
                                       (PVOID *)&gpPowerRequestCalloutEvent,
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        ERR("Failed to reference the power callout event handle (Status 0x%08lx)\n", Status);
#if (WINVER >= 0x0600)
        ExFreePoolWithTag(gpPowerSettingNotifyLock, USERTAG_POWER);
        gpPowerSettingNotifyLock = NULL;
#endif
        ExFreePoolWithTag(gpPowerCalloutMutexLock, USERTAG_POWER);
        return Status;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Cleanup procedure that frees all the allocated resources by
 * the power manager. It is triggered during Win32k subsystem unloading.
 */
NTSTATUS
NTAPI
IntWin32PowerManagementCleanup(VOID)
{
    PWIN32POWERCALLOUT pWin32PwrCallout;

    /* Dereference the power request event */
    ObDereferenceObject(gpPowerRequestCalloutEvent);
    gpPowerRequestCalloutEvent = NULL;

    /*
     * Enumerate all pending power callouts and free them. We do not
     * need to do this with the lock held as the CSR process is tore
     * apart during Win32k cleanup, so future power callouts would not
     * be allowed anyway, therefore we are safe.
     */
    while ((pWin32PwrCallout = IntGetNextPowerCallout()) != NULL)
    {
        ExFreePoolWithTag(pWin32PwrCallout, USERTAG_POWER);
    }

    /* Tear apart the power callout lock mutex */
    ExFreePoolWithTag(gpPowerCalloutMutexLock, USERTAG_POWER);
    gpPowerCalloutMutexLock = NULL;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Handles an incoming power event callout from the NT power
 * manager.
 *
 * @param[in] pWin32PwrEventParams
 * A pointer to power event parameters that is given by the
 * NT power manager of the kernel.
 *
 * @return
 * Returns STATUS_UNSUCCESSFUL if the Client/Server subsystem is
 * not running, of which power callouts cannot be handled.
 * Returns STATUS_INVALID_PARAMETER if the provided power event
 * parameters are not valid. Returns STATUS_INSUFFICIENT_RESOURCES
 * if there is a lack of memory to allocate for the power callout.
 * Otherwise it returns STATUS_SUCCESS to indicate the power callout
 * was handled successfully.
 */
NTSTATUS
NTAPI
IntHandlePowerEvent(
    _In_ PWIN32_POWEREVENT_PARAMETERS pWin32PwrEventParams)
{
    PWIN32POWERCALLOUT pWin32PwrCallout;
    NTSTATUS Status;

    /*
     * CSRSS is not running. As a consequence, the USER subsystem is neither
     * up and running as the Client/Server subsystem is responsible to fire
     * up other subsystems. Another case is that the system undergoes shutdown
     * and Win32k cleanup is currently in effect. Either way, just quit.
     */
    if (!gpepCSRSS)
    {
        TRACE("CSRSS is not running, bailing out\n");
        return STATUS_UNSUCCESSFUL;
    }

    /* Validate the power event parameters, just to be sure we have not gotten anything else */
    Status = IntValidateWin32PowerParams(pWin32PwrEventParams);
    if (!NT_SUCCESS(Status))
    {
        ERR("Could not deploy power callout, invalid Win32 power parameters!\n");
        return Status;
    }

    /* Allocate pool of memory for this power callout */
    pWin32PwrCallout = ExAllocatePoolZero(NonPagedPool,
                                          sizeof(WIN32POWERCALLOUT),
                                          USERTAG_POWER);
    if (pWin32PwrCallout == NULL)
    {
        ERR("Allocating memory for Win32 power callout failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Fill the necessary power datum */
    pWin32PwrCallout->Type = POWER_CALLOUT_EVENT;
    pWin32PwrCallout->Params.EventNumber = pWin32PwrEventParams->EventNumber;
    pWin32PwrCallout->Params.Code = pWin32PwrEventParams->Code;

    /* Enqueue this power request for later processing */
    IntEnlistPowerCallout(pWin32PwrCallout);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Handles an incoming power state callout from the NT power
 * manager.
 *
 * @param[in] pWin32PwrStateParams
 * A pointer to power state parameters that is given by the
 * NT power manager of the kernel.
 */
NTSTATUS
NTAPI
IntHandlePowerState(
    _In_ PWIN32_POWERSTATE_PARAMETERS pWin32PwrStateParams)
{
    /* FIXME */
    ERR("IntHandlePowerState is UNIMPLEMENTED\n");
    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Runs the power callouts that were queued by threads that could
 * not run them (system threads, attached or non-GUI threads). The
 * power request thread of CSRSS calls this each time the power
 * request event is signaled. The caller holds the USER lock.
 */
VOID
NTAPI
IntUserPowerCalloutWorker(VOID)
{
    IntDeployPowerCallout();
}

#if (WINVER >= 0x0600)
/**
 * @brief
 * Registers a window for the power setting notifications of a
 * setting (RegisterPowerSettingNotification). The window gets
 * WM_POWERBROADCAST PBT_POWERSETTINGCHANGE with the current value
 * right away and again whenever the value changes.
 *
 * @return
 * The notification handle, or 0 with the last error set.
 */
ULONG_PTR
NTAPI
IntRegisterPowerSettingNotification(
    _In_ PWND pWnd,
    _In_ LPCGUID UserGuid)
{
    PUSER_POWER_SETTING_NOTIFY pNotify;
    GUID Guid;
    LPCGUID KernelGuid;
    NTSTATUS Status;

    ASSERT(UserIsEnteredExclusive());

    _SEH2_TRY
    {
        ProbeForRead(UserGuid, sizeof(GUID), 1);
        Guid = *UserGuid;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        EngSetLastError(ERROR_NOACCESS);
        _SEH2_YIELD(return 0);
    }
    _SEH2_END;

    /* The display of the (only) session is the console display */
    KernelGuid = IsEqualGUID(&Guid, &GUID_SESSION_DISPLAY_STATUS_) ? &GUID_CONSOLE_DISPLAY_STATE_ : &Guid;

    pNotify = ExAllocatePoolZero(PagedPool, sizeof(*pNotify), USERTAG_POWER);
    if (pNotify == NULL)
    {
        EngSetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    pNotify->ppi = GetW32ProcessInfo();
    pNotify->hwnd = UserHMGetHandle(pWnd);
    pNotify->Guid = Guid;

    KeEnterCriticalRegion();
    ExAcquireFastMutexUnsafe(gpPowerSettingNotifyLock);
    do
    {
        pNotify->Notify = ++gLastPowerSettingNotify;
    } while (pNotify->Notify == 0);
    ExReleaseFastMutexUnsafe(gpPowerSettingNotifyLock);
    KeLeaveCriticalRegion();

    /* The callback only reads the registration, it may run before this returns */
    Status = PoRegisterPowerSettingCallback(NULL,
                                            KernelGuid,
                                            IntPowerSettingCallback,
                                            pNotify,
                                            &pNotify->PoHandle);
    if (!NT_SUCCESS(Status))
    {
        ERR("Cannot register window %p for power setting %08lx (0x%08lx)\n",
            pNotify->hwnd, Guid.Data1, Status);
        ExFreePoolWithTag(pNotify, USERTAG_POWER);
        SetLastNtError(Status);
        return 0;
    }

    KeEnterCriticalRegion();
    ExAcquireFastMutexUnsafe(gpPowerSettingNotifyLock);
    InsertTailList(&gPowerSettingNotifyList, &pNotify->Link);
    ExReleaseFastMutexUnsafe(gpPowerSettingNotifyLock);
    KeLeaveCriticalRegion();

    return pNotify->Notify;
}

static
VOID
IntFreePowerSettingNotification(
    _In_ PUSER_POWER_SETTING_NOTIFY pNotify)
{
    NTSTATUS Status;

    /* Waits for a callback that is running; changes reported before stay queued */
    Status = PoUnregisterPowerSettingCallback(pNotify->PoHandle);
    ASSERT(NT_SUCCESS(Status));
    ExFreePoolWithTag(pNotify, USERTAG_POWER);
}

/* Takes a registration of the process off the list; Notify == 0 takes any */
static
PUSER_POWER_SETTING_NOTIFY
IntRemovePowerSettingNotification(
    _In_ PPROCESSINFO ppi,
    _In_ ULONG_PTR Notify)
{
    PUSER_POWER_SETTING_NOTIFY pNotify = NULL;
    PLIST_ENTRY Entry;

    KeEnterCriticalRegion();
    ExAcquireFastMutexUnsafe(gpPowerSettingNotifyLock);
    for (Entry = gPowerSettingNotifyList.Flink;
         Entry != &gPowerSettingNotifyList;
         Entry = Entry->Flink)
    {
        PUSER_POWER_SETTING_NOTIFY pEntry = CONTAINING_RECORD(Entry, USER_POWER_SETTING_NOTIFY, Link);
        if (pEntry->ppi == ppi && (Notify == 0 || pEntry->Notify == Notify))
        {
            RemoveEntryList(&pEntry->Link);
            pNotify = pEntry;
            break;
        }
    }
    ExReleaseFastMutexUnsafe(gpPowerSettingNotifyLock);
    KeLeaveCriticalRegion();

    return pNotify;
}

/**
 * @brief
 * Removes a registration of the calling process
 * (UnregisterPowerSettingNotification).
 */
BOOL
NTAPI
IntUnregisterPowerSettingNotification(
    _In_ ULONG_PTR Notify)
{
    PUSER_POWER_SETTING_NOTIFY pNotify;

    pNotify = Notify ? IntRemovePowerSettingNotification(GetW32ProcessInfo(), Notify) : NULL;
    if (pNotify == NULL)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    IntFreePowerSettingNotification(pNotify);
    return TRUE;
}

/**
 * @brief
 * Removes the registrations of a process that goes away.
 */
VOID
NTAPI
IntCleanupPowerSettingNotifications(
    _In_ PPROCESSINFO ppi)
{
    PUSER_POWER_SETTING_NOTIFY pNotify;

    /* Power management is not up before CSRSS initialized USER */
    if (gpPowerSettingNotifyLock == NULL)
        return;

    while ((pNotify = IntRemovePowerSettingNotification(ppi, 0)) != NULL)
        IntFreePowerSettingNotification(pNotify);
}
#else
ULONG_PTR
NTAPI
IntRegisterPowerSettingNotification(
    _In_ PWND pWnd,
    _In_ LPCGUID UserGuid)
{
    EngSetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

BOOL
NTAPI
IntUnregisterPowerSettingNotification(
    _In_ ULONG_PTR Notify)
{
    EngSetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

VOID
NTAPI
IntCleanupPowerSettingNotifications(
    _In_ PPROCESSINFO ppi)
{
}
#endif

/* EOF */
