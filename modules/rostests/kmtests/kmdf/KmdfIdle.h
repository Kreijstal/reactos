/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KMDF system-managed idle test declarations
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#pragma once

/*
 * kmdfidle_drv is a KMDF function driver for a root-enumerated device that
 * leaves its idle timeout to the power framework (SystemManagedIdleTimeout-
 * WithHint) and hands KMDF its own power framework callbacks with
 * WdfDeviceWdmAssignPowerFrameworkSettings. It records, in order, the
 * power framework and power callbacks it gets; user mode reads that log
 * through \\.\KmtKmdfIdle without waking the device up.
 */

#define KMDFIDLE_SERVICE_NAME       L"KmtKmdfIdle"
#define KMDFIDLE_IMAGE_NAME         L"kmdfidle_drv.sys"
#define KMDFIDLE_DEVICE_ID          L"KmtKmdfIdle"
#define KMDFIDLE_DOS_NAME           L"\\\\.\\KmtKmdfIdle"

/* The idle timeout hint given to KMDF, in milliseconds */
#define KMDFIDLE_TIMEOUT_MS         200

/* Returns the KMDFIDLE_LOG */
#define IOCTL_KMDFIDLE_GET_LOG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)
/* WdfDeviceStopIdle(WaitForD0 = TRUE) and WdfDeviceResumeIdle: one wake-up */
#define IOCTL_KMDFIDLE_WAKE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_WRITE_ACCESS)

typedef enum _KMDFIDLE_EVENT
{
    KmdfIdleEventNone,
    KmdfIdleEventPostRegister,      /* EvtDeviceWdmPostPoFxRegisterDevice */
    KmdfIdleEventActive,            /* ComponentActiveConditionCallback */
    KmdfIdleEventIdle,              /* ComponentIdleConditionCallback */
    KmdfIdleEventD0Entry,
    KmdfIdleEventD0Exit,
    KmdfIdleEventPreUnregister,     /* EvtDeviceWdmPrePoFxUnregisterDevice */
    KmdfIdleEventWakeDone,          /* IOCTL_KMDFIDLE_WAKE found the device in D0 */
} KMDFIDLE_EVENT;

#define KMDFIDLE_MAX_EVENTS 64

typedef struct _KMDFIDLE_LOG
{
    /* The kernel has a power framework (PoFxCompleteIdleCondition exported) */
    ULONG PoFxPresent;
    /* EvtDeviceWdmPostPoFxRegisterDevice got a non-NULL POHANDLE */
    ULONG HandleValid;
    /* Callbacks that got a wrong context or component number */
    ULONG BadArguments;
    ULONG Count;
    UCHAR Events[KMDFIDLE_MAX_EVENTS];
} KMDFIDLE_LOG, *PKMDFIDLE_LOG;
