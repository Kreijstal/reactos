/*
 * PROJECT:     ReactOS Configuration Manager
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Device notification availability
 */

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <cfgmgr32.h>

CONFIGRET
WINAPI
CM_Register_Notification(PVOID Filter, PVOID Context, PVOID Callback, PVOID *Notification)
{
    UNREFERENCED_PARAMETER(Context);
    if (!Filter || !Callback || !Notification)
        return CR_INVALID_POINTER;
    *Notification = NULL;
    /* User-mode Configuration Manager callbacks are not implemented yet. */
    return CR_CALL_NOT_IMPLEMENTED;
}

CONFIGRET
WINAPI
CM_Unregister_Notification(PVOID Notification)
{
    return Notification ? CR_INVALID_DATA : CR_INVALID_POINTER;
}
