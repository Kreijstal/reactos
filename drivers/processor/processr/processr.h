/*
 * PROJECT:        ReactOS Generic CPU Driver
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * FILE:           drivers/processor/processr/processr.h
 * PURPOSE:        Common header file
 * PROGRAMMERS:    Eric Kohl <eric.kohl@reactos.org>
 */

#ifndef _PROCESSR_PCH_
#define _PROCESSR_PCH_

#include <ntddk.h>

typedef struct _DEVICE_EXTENSION
{
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_OBJECT LowerDevice;

} DEVICE_EXTENSION, *PDEVICE_EXTENSION;


/* misc.c */

NTSTATUS
NTAPI
ForwardIrpAndForget(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp);


/* pnp.c */

NTSTATUS
NTAPI
ProcessorPnp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp);

NTSTATUS
NTAPI
ProcessorAddDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT Pdo);

/* pstate.c */

/*
 * Exported by ntoskrnl (ntoskrnl.spec) but not declared in any public DDK
 * header. The governor uses it to sample per-processor idle/busy time.
 */
NTKERNELAPI
VOID
NTAPI
ExGetCurrentProcessorCounts(
    _Out_ PULONG IdleTime,
    _Out_ PULONG KernelAndUserTime,
    _Out_ PULONG ProcessorNumber);

/*
 * ACPI 6.x limits _PSS to 16 entries in practice; the AMD software P-state
 * MSR range (MSRC001_0064..006B) holds at most 8.
 */
#define PROCESSOR_MAX_PERF_STATES 16

/* PROCESSOR_PERF_STATE is taken by ntpoapi.h, hence the driver specific name */
typedef struct _PROCESSR_PSTATE
{
    ULONG CoreFrequency;        /* MHz */
    ULONG Power;                /* mW */
    ULONG TransitionLatency;    /* us */
    ULONG BusMasterLatency;     /* us */
    ULONG Control;              /* Value written to the control register */
    ULONG Status;               /* Value the status register reports afterwards */
} PROCESSR_PSTATE, *PPROCESSR_PSTATE;

VOID
ProcessorPerfInitialize(
    _In_ PDEVICE_OBJECT DeviceObject);

VOID
ProcessorPerfStop(
    _In_opt_ PDEVICE_OBJECT DeviceObject);

#endif /* _PROCESSR_PCH_ */
