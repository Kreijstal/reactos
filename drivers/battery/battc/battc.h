/*
* PROJECT:         ReactOS Kernel
* LICENSE:         GPL - See COPYING in the top level directory
* FILE:            drivers/battery/battc/battc.h
* PURPOSE:         Battery Class Driver
* PROGRAMMERS:     Cameron Gutman (cameron.gutman@reactos.org)
*/

#pragma once

#include <ntddk.h>
#include <wmilib.h>
#include <initguid.h>
#include <batclass.h>

typedef struct _BATTERY_CLASS_DATA
{
    BATTERY_MINIPORT_INFO MiniportInfo;
    UNICODE_STRING InterfaceName;

    /* Pending IOCTL_BATTERY_QUERY_TAG and IOCTL_BATTERY_QUERY_STATUS waits */
    KSPIN_LOCK WaitLock;
    LIST_ENTRY TagWaits;
    LIST_ENTRY StatusWaits;

    /* The wait worker: re-checks the pending waits at PASSIVE_LEVEL */
    WORK_QUEUE_ITEM WaitWorkItem;
    BOOLEAN WorkerQueued;
    BOOLEAN WorkerRunning;
    BOOLEAN WorkerRerun;
    BOOLEAN Unloading;
    KEVENT WorkerIdleEvent;

    /* Fires at the earliest wait time-out or poll time */
    KTIMER WaitTimer;
    KDPC WaitTimerDpc;

    /* What the miniport was last asked to notify about (worker only) */
    BOOLEAN NotifySet;
    BOOLEAN NotifyFailed;
    ULONG NotifyTag;
    BATTERY_NOTIFY Notify;
} BATTERY_CLASS_DATA, *PBATTERY_CLASS_DATA;

/* A pending wait, in Irp->Tail.Overlay.DriverContext[0] */
typedef struct _BATTERY_WAIT_CONTEXT
{
    BATTERY_WAIT_STATUS Wait;
    /* Interrupt time at which the wait ends; MAXULONGLONG for none */
    ULONGLONG Deadline;
} BATTERY_WAIT_CONTEXT, *PBATTERY_WAIT_CONTEXT;

/* Memory tags */
#define BATTERY_CLASS_DATA_TAG 'CtaB'
#define BATTERY_WAIT_TAG       'WtaB'

/* How often a battery whose miniport cannot notify is polled for a wait */
#define BATTERY_POLL_INTERVAL_MS 5000
