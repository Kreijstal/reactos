/*
 * PROJECT:     ReactOS hardware bring-up poke driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Host stand-ins for the DDK, so the driver's logic can be tested
 *              without the machine it is meant to debug
 * COPYRIGHT:   Copyright 2026 the ReactOS contributors
 *
 * Only the facilities rospoke actually uses are modelled, and they are modelled
 * as data the test can inspect: config space is an array the test can read back
 * (so "did BAR sizing put the BARs back?" is answerable), and MMIO reads go
 * through a function so a register can be scripted to change after N reads (so
 * poll semantics are testable without a device).
 *
 * This file is not part of any ReactOS build.  See README.md.
 */

#ifndef _ROSPOKE_KSTUBS_H_
#define _ROSPOKE_KSTUBS_H_

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

typedef void VOID;
typedef void *PVOID;
typedef unsigned char UCHAR, *PUCHAR;
typedef unsigned short USHORT, *PUSHORT;
typedef unsigned int ULONG, *PULONG;
typedef int LONG;
typedef char CHAR;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
typedef uintptr_t ULONG_PTR;
typedef size_t SIZE_T;
typedef unsigned char BOOLEAN;
typedef long NTSTATUS;

#define TRUE  1
#define FALSE 0
#ifndef NULL
#define NULL ((void *)0)
#endif

typedef union _LARGE_INTEGER
{
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef LARGE_INTEGER PHYSICAL_ADDRESS;

#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BBL)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000DL)
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009AL)
#define STATUS_DEVICE_NOT_READY         ((NTSTATUS)0xC00000A3L)
#define STATUS_DEVICE_DOES_NOT_EXIST    ((NTSTATUS)0xC00000C0L)
#define STATUS_OBJECT_TYPE_MISMATCH     ((NTSTATUS)0xC0000024L)
#define STATUS_DATATYPE_MISALIGNMENT    ((NTSTATUS)0x80000002L)
#define STATUS_INVALID_BUFFER_SIZE      ((NTSTATUS)0xC0000206L)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)
#define STATUS_INVALID_DEVICE_REQUEST   ((NTSTATUS)0xC0000010L)

#define NT_SUCCESS(x) (((NTSTATUS)(x)) >= 0)

/* SAL is documentation here, not analysis. */
#define _In_
#define _Out_
#define _Inout_
#define _In_opt_
#define _Out_opt_
#define _In_reads_(x)
#define _In_reads_bytes_(x)
#define _Out_writes_bytes_(x)
#define _Out_writes_to_(x, y)

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#define UNREFERENCED_PARAMETER(x) ((void)(x))

#define RtlZeroMemory(d, l) memset((d), 0, (l))
#define RtlCopyMemory(d, s, l) memcpy((d), (s), (l))

typedef enum _MEMORY_CACHING_TYPE
{
    MmNonCached = 0,
    MmCached = 1,
    MmWriteCombined = 2
} MEMORY_CACHING_TYPE;

typedef struct _KMUTEX { int Unused; } KMUTEX;

typedef enum _KPROCESSOR_MODE { KernelMode = 0, UserMode = 1 } KPROCESSOR_MODE;

typedef enum _BUS_DATA_TYPE { PCIConfiguration = 4 } BUS_DATA_TYPE;

typedef struct _PCI_SLOT_NUMBER
{
    union
    {
        struct
        {
            ULONG DeviceNumber : 5;
            ULONG FunctionNumber : 3;
            ULONG Reserved : 24;
        } bits;
        ULONG AsULONG;
    } u;
} PCI_SLOT_NUMBER;

#define DPRINT1(...) RospokeHarnessLog(__VA_ARGS__)
void RospokeHarnessLog(const char *Format, ...);

/* --- timing ------------------------------------------------------------- */

LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER *Frequency);
void KeStallExecutionProcessor(ULONG Micros);
NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE Mode, BOOLEAN Alertable, LARGE_INTEGER *Interval);

/* --- register access ---------------------------------------------------- */

ULONG READ_REGISTER_ULONG(volatile ULONG *Address);
USHORT READ_REGISTER_USHORT(volatile USHORT *Address);
UCHAR READ_REGISTER_UCHAR(volatile UCHAR *Address);
void WRITE_REGISTER_ULONG(volatile ULONG *Address, ULONG Value);
void WRITE_REGISTER_USHORT(volatile USHORT *Address, USHORT Value);
void WRITE_REGISTER_UCHAR(volatile UCHAR *Address, UCHAR Value);
void READ_REGISTER_BUFFER_ULONG(volatile ULONG *Address, PULONG Buffer, ULONG Count);
void READ_REGISTER_BUFFER_USHORT(volatile USHORT *Address, PUSHORT Buffer, ULONG Count);
void READ_REGISTER_BUFFER_UCHAR(volatile UCHAR *Address, PUCHAR Buffer, ULONG Count);
void WRITE_REGISTER_BUFFER_ULONG(volatile ULONG *Address, PULONG Buffer, ULONG Count);
void WRITE_REGISTER_BUFFER_USHORT(volatile USHORT *Address, PUSHORT Buffer, ULONG Count);
void WRITE_REGISTER_BUFFER_UCHAR(volatile UCHAR *Address, PUCHAR Buffer, ULONG Count);

/* --- memory ------------------------------------------------------------- */

PVOID MmMapIoSpace(PHYSICAL_ADDRESS Physical, SIZE_T Length, MEMORY_CACHING_TYPE Cache);
void MmUnmapIoSpace(PVOID Base, SIZE_T Length);
PVOID MmAllocateContiguousMemorySpecifyCache(SIZE_T Length,
                                             PHYSICAL_ADDRESS Low,
                                             PHYSICAL_ADDRESS High,
                                             PHYSICAL_ADDRESS Boundary,
                                             MEMORY_CACHING_TYPE Cache);
void MmFreeContiguousMemorySpecifyCache(PVOID Base, SIZE_T Length, MEMORY_CACHING_TYPE Cache);
PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID Base);

/* --- PCI config space --------------------------------------------------- */

ULONG HalGetBusDataByOffset(BUS_DATA_TYPE Type, ULONG Bus, ULONG Slot,
                            PVOID Buffer, ULONG Offset, ULONG Length);
ULONG HalSetBusDataByOffset(BUS_DATA_TYPE Type, ULONG Bus, ULONG Slot,
                            PVOID Buffer, ULONG Offset, ULONG Length);

/* --- knobs the tests drive --------------------------------------------- */

#define HARNESS_CFG_SIZE 256

/* The fake device's config space, readable and writable by the test so it can
 * assert that BAR sizing put everything back the way it found it. */
extern unsigned char HarnessConfigSpace[HARNESS_CFG_SIZE];
extern int HarnessConfigPresent;
extern unsigned long HarnessConfigWrites;

/* A register whose value changes after a set number of reads, so a poll can be
 * made to succeed on the Nth attempt or never. */
void HarnessScriptRegister(void *Address, ULONG Before, ULONG After, ULONG ReadsBeforeChange);
void HarnessClearScripts(void);

/* Physical-address model: register a window before the code under test maps it. */
void HarnessAddRegion(ULONGLONG Physical, SIZE_T Length);
void HarnessResetRegions(void);
void *HarnessRegionBase(ULONGLONG Physical);

extern unsigned long HarnessStalledUs;
extern unsigned long HarnessMmioReads;
extern unsigned long HarnessMmioWrites;
extern int HarnessQuiet;

#endif /* _ROSPOKE_KSTUBS_H_ */
