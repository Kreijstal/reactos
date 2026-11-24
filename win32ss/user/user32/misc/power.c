/*
 * PROJECT:     ReactOS user32.dll
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Power setting notifications
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include <user32.h>

WINE_DEFAULT_DEBUG_CHANNEL(user32);

/*
 * A window registered for a power setting gets WM_POWERBROADCAST with
 * PBT_POWERSETTINGCHANGE and a POWERBROADCAST_SETTING in lParam: once with
 * the current value right after registering, then on every change.
 *
 * @implemented (window recipients)
 */
PVOID
WINAPI
RegisterPowerSettingNotification(
    _In_ HANDLE hRecipient,
    _In_ LPCGUID PowerSettingGuid,
    _In_ DWORD Flags)
{
    switch (Flags)
    {
        case DEVICE_NOTIFY_WINDOW_HANDLE:
            return (PVOID)NtUserCallTwoParam((DWORD_PTR)hRecipient,
                                             (DWORD_PTR)PowerSettingGuid,
                                             TWOPARAM_ROUTINE_REGISTERPOWERSETTINGNOTIFY);

        case DEVICE_NOTIFY_SERVICE_HANDLE:
            /* The SCM does not forward SERVICE_CONTROL_POWEREVENT yet */
            FIXME("(%p, %s): service recipients are not supported\n",
                  hRecipient, debugstr_guid(PowerSettingGuid));
            SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
            return NULL;

        default:
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
    }
}

/*
 * @implemented
 */
BOOL
WINAPI
UnregisterPowerSettingNotification(
    _In_ PVOID Handle)
{
    return (BOOL)NtUserCallOneParam((DWORD_PTR)Handle,
                                    ONEPARAM_ROUTINE_UNREGISTERPOWERSETTINGNOTIFY);
}
