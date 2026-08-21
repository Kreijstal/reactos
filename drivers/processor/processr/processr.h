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
#include <acpiioct.h>

#define PROCESSOR_TAG 'PcrP'

/* ACPI address space identifiers used by the Register() descriptors in
 * _PCT (P-state control) and _CST (C-state entry) */
#define ACPI_ADDRESS_SPACE_SYSTEM_MEMORY 0x00
#define ACPI_ADDRESS_SPACE_SYSTEM_IO     0x01
#define ACPI_ADDRESS_SPACE_FIXED_HW      0x7F

/* Large resource descriptor tag of Register() */
#define ACPI_GENERIC_REGISTER_TAG 0x82

#include <pshpack1.h>
typedef struct _ACPI_GENERIC_REGISTER
{
    UCHAR Tag;
    USHORT Length;
    UCHAR AddressSpaceId;
    UCHAR BitWidth;
    UCHAR BitOffset;
    UCHAR AccessSize;
    ULONGLONG Address;
} ACPI_GENERIC_REGISTER, *PACPI_GENERIC_REGISTER;
#include <poppack.h>

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

/* pstate.c -- generic ACPI plumbing, shared with cstate.c */

/*
 * Evaluates a control method on the ACPI stack below us and returns the
 * (pool allocated) output buffer on success. The caller frees it.
 */
NTSTATUS
ProcessorAcpiEvaluateMethod(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PCSTR MethodName,
    _Outptr_result_nullonfailure_ PACPI_EVAL_OUTPUT_BUFFER *ReturnBuffer);

/*
 * TRUE when an argument, and the data it claims to carry, lie entirely inside
 * the buffer ACPI actually filled.
 */
BOOLEAN
ProcessorAcpiArgumentFits(
    _In_ PACPI_EVAL_OUTPUT_BUFFER Buffer,
    _In_ PACPI_METHOD_ARGUMENT Argument);

/* cstate.c */

VOID
ProcessorIdleInitialize(
    _In_ PDEVICE_OBJECT DeviceObject);

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
