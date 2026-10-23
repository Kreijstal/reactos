/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KMDF device removal test declarations
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

/*
 * kmdfremove_drv is a plain KMDF function driver for a root-enumerated
 * device. User mode opens it through \\.\KmtKmdfRemove and asks it to
 * report itself gone or failed; PnP then removes the device and unloads
 * the driver. The driver counts its callbacks in its service key, under
 * Parameters, so that user mode can read them once the driver is gone.
 */

#define KMDFREMOVE_SERVICE_NAME     L"KmtKmdfRemove"
#define KMDFREMOVE_IMAGE_NAME       L"kmdfremove_drv.sys"
#define KMDFREMOVE_DEVICE_ID        L"KmtKmdfRemove"
#define KMDFREMOVE_DOS_NAME         L"\\\\.\\KmtKmdfRemove"

/* WdfDeviceSetDeviceState(Removed = WdfTrue) */
#define IOCTL_KMDFREMOVE_REPORT_REMOVED \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_WRITE_ACCESS)
/* WdfDeviceSetDeviceState(Failed = WdfTrue) */
#define IOCTL_KMDFREMOVE_REPORT_FAILED \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_WRITE_ACCESS)
/* WdfDeviceSetFailed(WdfDeviceFailedNoRestart) */
#define IOCTL_KMDFREMOVE_SET_FAILED \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/* REG_DWORD values under Services\KmtKmdfRemove\Parameters */
#define KMDFREMOVE_VALUE_DEVICE_ADDS        L"DeviceAdds"
#define KMDFREMOVE_VALUE_D0_ENTRIES         L"D0Entries"
#define KMDFREMOVE_VALUE_D0_EXITS           L"D0Exits"
#define KMDFREMOVE_VALUE_QUERY_REMOVES      L"QueryRemoves"
#define KMDFREMOVE_VALUE_SURPRISE_REMOVALS  L"SurpriseRemovals"
#define KMDFREMOVE_VALUE_RELEASE_HARDWARES  L"ReleaseHardwares"
#define KMDFREMOVE_VALUE_FILE_CLEANUPS      L"FileCleanups"
#define KMDFREMOVE_VALUE_DEVICE_CLEANUPS    L"DeviceCleanups"
#define KMDFREMOVE_VALUE_DEVICE_DESTROYS    L"DeviceDestroys"
#define KMDFREMOVE_VALUE_CHILD_CLEANUPS     L"ChildCleanups"
#define KMDFREMOVE_VALUE_UNLOADS            L"Unloads"
