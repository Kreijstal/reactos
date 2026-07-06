/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Info Classes for the Process Manager
 * COPYRIGHT:   Copyright Alex Ionescu <alex.ionescu@reactos.org>
 *              Copyright Thomas Weidenmueller <w3seek@reactos.org>
 *              Copyright 2020-2021 George Bișoc <george.bisoc@reactos.org>
 */

#include "icif.h"

typedef struct _PS_PROCESS_CYCLE_TIME_INFORMATION
{
    ULONGLONG AccumulatedCycles;
    ULONGLONG CurrentCycleCount;
} PS_PROCESS_CYCLE_TIME_INFORMATION, *PPS_PROCESS_CYCLE_TIME_INFORMATION;

typedef struct _PS_PAGE_PRIORITY_INFORMATION
{
    ULONG PagePriority;
} PS_PAGE_PRIORITY_INFORMATION, *PPS_PAGE_PRIORITY_INFORMATION;

typedef struct _PS_PROCESS_INSTRUMENTATION_CALLBACK_INFORMATION
{
    ULONG Version;
    ULONG Reserved;
    PVOID Callback;
} PS_PROCESS_INSTRUMENTATION_CALLBACK_INFORMATION, *PPS_PROCESS_INSTRUMENTATION_CALLBACK_INFORMATION;

typedef struct _PS_PROCESS_STACK_ALLOCATION_INFORMATION
{
    SIZE_T ReserveSize;
    SIZE_T ZeroBits;
    PVOID StackBase;
} PS_PROCESS_STACK_ALLOCATION_INFORMATION, *PPS_PROCESS_STACK_ALLOCATION_INFORMATION;

//
// Process Information Classes
//
static const INFORMATION_CLASS_INFO PsProcessInfoClass[] =
{
    /* ProcessBasicInformation */
    IQS_SAME
    (
        PROCESS_BASIC_INFORMATION,
        ULONG,
        ICIF_QUERY
    ),

    /* ProcessQuotaLimits */
    IQS_SAME
    (
        QUOTA_LIMITS,
        ULONG,

        /* NOTE: ICIF_SIZE_VARIABLE is for QUOTA_LIMITS_EX support */
        ICIF_QUERY | ICIF_SET | ICIF_SIZE_VARIABLE
    ),

    /* ProcessIoCounters */
    IQS_SAME
    (
        IO_COUNTERS,
        ULONG,
        ICIF_QUERY
    ),

    /* ProcessVmCounters */
    IQS_SAME
    (
        VM_COUNTERS,
        ULONG,
        ICIF_QUERY | ICIF_QUERY_SIZE_VARIABLE
    ),

    /* ProcessTimes */
    IQS_SAME
    (
        KERNEL_USER_TIMES,
        ULONG,
        ICIF_QUERY
    ),

    /* ProcessBasePriority */
    IQS_SAME
    (
        KPRIORITY,
        ULONG,
        ICIF_SET
    ),

    /* ProcessRaisePriority */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_SET
    ),

    /* ProcessDebugPort */
    IQS_SAME
    (
        HANDLE,
        ULONG,
        ICIF_QUERY
    ),

    /* ProcessExceptionPort */
    IQS_SAME
    (
        HANDLE,
        HANDLE,
        ICIF_SET
    ),

    /* ProcessAccessToken */
    IQS_SAME
    (
        PROCESS_ACCESS_TOKEN,
        ULONG,
        ICIF_SET
    ),

    /* ProcessLdtInformation */
    IQS_SAME
    (
        PROCESS_LDT_INFORMATION,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessLdtSize */
    IQS_SAME
    (
        PROCESS_LDT_SIZE,
        ULONG,
        ICIF_SET
    ),

    /* ProcessDefaultHardErrorMode */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessIoPortHandlers */
    IQS_SAME
    (
        UCHAR,
        ULONG,
        ICIF_SET
    ),

    /* ProcessPooledUsageAndLimits */
    IQS_SAME
    (
        POOLED_USAGE_AND_LIMITS,
        ULONG,
        ICIF_QUERY
    ),

    /* ProcessWorkingSetWatch */
    IQS_SAME
    (
        PROCESS_WS_WATCH_INFORMATION,
        ULONG,
        ICIF_QUERY | ICIF_SET | ICIF_SET_SIZE_VARIABLE
    ),

    /* ProcessUserModeIOPL is only implemented in x86 */
#if defined (_X86_)
    IQS_NO_TYPE_LENGTH
    (
        ULONG,
        ICIF_SET
    ),
#else
    IQS_NONE,
#endif

    /* ProcessEnableAlignmentFaultFixup */
    IQS
    (
        BOOLEAN,
        ULONG,
        BOOLEAN,
        CHAR,
        ICIF_SET
    ),

    /* ProcessPriorityClass */
    IQS
    (
        PROCESS_PRIORITY_CLASS,
        ULONG,
        PROCESS_PRIORITY_CLASS,
        CHAR,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessWx86Information */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessHandleCount */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY
    ),

    /* ProcessAffinityMask */
    IQS_SAME
    (
        KAFFINITY,
        KAFFINITY,
        ICIF_SET
    ),

    /* ProcessPriorityBoost */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessDeviceMap */
    IQS
    (
        RTL_FIELD_TYPE(PROCESS_DEVICEMAP_INFORMATION, Query),
        ULONG,
        RTL_FIELD_TYPE(PROCESS_DEVICEMAP_INFORMATION, Set),
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessSessionInformation */
    IQS_SAME
    (
        PROCESS_SESSION_INFORMATION,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessForegroundInformation */
    IQS
    (
        CHAR,
        ULONG,
        BOOLEAN,
        CHAR,
        ICIF_SET
    ),

    /* ProcessWow64Information */
    IQS_SAME
    (
        ULONG_PTR,
        ULONG,
        ICIF_QUERY
    ),

    /* ProcessImageFileName */
    IQS_SAME
    (
        UNICODE_STRING,
        ULONG,
        ICIF_QUERY | ICIF_QUERY_SIZE_VARIABLE
    ),

    /* ProcessLUIDDeviceMapsEnabled */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY
    ),

    /* ProcessBreakOnTermination */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessDebugObjectHandle */
    IQS_SAME
    (
        HANDLE,
        ULONG,
        ICIF_QUERY
    ),

    /* ProcessDebugFlags */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessHandleTracing */
    IQS
    (
        PROCESS_HANDLE_TRACING_QUERY,
        ULONG,
        ULONG,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessIoPriority */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessExecuteFlags */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessTlsInformation */
    IQS_SAME
    (
        ULONG_PTR,
        ULONG_PTR,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessCookie */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY
    ),

    /* ProcessImageInformation */
    IQS_SAME
    (
        SECTION_IMAGE_INFORMATION,
        ULONG,
        ICIF_QUERY
    ),

    /* ProcessCycleTime */
    IQS_SAME
    (
        PS_PROCESS_CYCLE_TIME_INFORMATION,
        ULONGLONG,
        ICIF_QUERY
    ),

    /* ProcessPagePriority */
    IQS_SAME
    (
        PS_PAGE_PRIORITY_INFORMATION,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessInstrumentationCallback */
    IQS
    (
        PS_PROCESS_INSTRUMENTATION_CALLBACK_INFORMATION,
        ULONG_PTR,
        PS_PROCESS_INSTRUMENTATION_CALLBACK_INFORMATION,
        ULONG_PTR,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessThreadStackAllocation */
    IQS
    (
        PS_PROCESS_STACK_ALLOCATION_INFORMATION,
        ULONG_PTR,
        PS_PROCESS_STACK_ALLOCATION_INFORMATION,
        ULONG_PTR,
        ICIF_QUERY | ICIF_SET | ICIF_SET_SIZE_VARIABLE
    ),

    /* ProcessWorkingSetWatchEx */
    IQS_SAME
    (
        PROCESS_WS_WATCH_INFORMATION,
        ULONG_PTR,
        ICIF_QUERY | ICIF_SET | ICIF_SET_SIZE_VARIABLE
    ),

    /* ProcessImageFileNameWin32 */
    IQS_SAME
    (
        UNICODE_STRING,
        ULONG_PTR,
        ICIF_QUERY | ICIF_QUERY_SIZE_VARIABLE
    ),

    /* ProcessImageFileMapping */
    IQS_SAME
    (
        HANDLE,
        ULONG_PTR,
        ICIF_QUERY
    ),

    /* ProcessAffinityUpdateMode */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessMemoryAllocationMode */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ProcessGroupInformation */
    IQS_NONE,

    /* ProcessTokenVirtualizationEnabled */
    IQS_NONE,

    /* ProcessConsoleHostProcess */
    IQS_NONE,

    /* ProcessWindowInformation */
    IQS_NONE,

    /* ProcessHandleInformation */
    IQS_SAME
    (
        PROCESS_HANDLE_SNAPSHOT_INFORMATION,
        ULONG_PTR,
        ICIF_QUERY | ICIF_QUERY_SIZE_VARIABLE
    ),
};

//
// Thread Information Classes
//
static const INFORMATION_CLASS_INFO PsThreadInfoClass[] =
{
    /* ThreadBasicInformation */
    IQS_SAME
    (
        THREAD_BASIC_INFORMATION,
        ULONG,
        ICIF_QUERY
    ),

    /* ThreadTimes */
    IQS_SAME
    (
        KERNEL_USER_TIMES,
        ULONG,
        ICIF_QUERY
    ),

    /* ThreadPriority */
    IQS_SAME
    (
        KPRIORITY,
        ULONG,
        ICIF_SET
    ),

    /* ThreadBasePriority */
    IQS_SAME
    (
        LONG,
        ULONG,
        ICIF_SET
    ),

    /* ThreadAffinityMask */
    IQS_SAME
    (
        KAFFINITY,
        ULONG,
        ICIF_SET
    ),

    /* ThreadImpersonationToken */
    IQS_SAME
    (
        HANDLE,
        ULONG,
        ICIF_SET
    ),

    /* ThreadDescriptorTableEntry is only implemented in x86 as well as the descriptor entry */
#if defined(_X86_)
    IQS_SAME
    (
        DESCRIPTOR_TABLE_ENTRY,
        ULONG,
        ICIF_QUERY
    ),
#else
    IQS_NONE,
#endif

    /* ThreadEnableAlignmentFaultFixup */
    IQS
    (
        CHAR,
        CHAR,
        BOOLEAN,
        UCHAR,
        ICIF_SET
    ),

    /* ThreadEventPair_Reusable */
    IQS_NONE,

    /* ThreadQuerySetWin32StartAddress */
    IQS
    (
        PVOID,
        ULONG,
        ULONG_PTR,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ThreadZeroTlsCell */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_SET
    ),

    /* ThreadPerformanceCount */
    IQS_SAME
    (
        LARGE_INTEGER,
        ULONG,
        ICIF_QUERY
    ),

    /* ThreadAmILastThread */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY
    ),

    /* ThreadIdealProcessor */
    IQS_SAME
    (
        ULONG_PTR,
        ULONG,
        ICIF_SET
    ),

    /* ThreadPriorityBoost */
    IQS
    (
        ULONG,
        ULONG,
        ULONG_PTR,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ThreadSetTlsArrayAddress */
    IQS_SAME
    (
        PVOID,
        ULONG,
        ICIF_SET | ICIF_SIZE_VARIABLE
    ),

    /* ThreadIsIoPending */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY
    ),

    /* ThreadHideFromDebugger */
    {
#if (NTDDI_VERSION >= NTDDI_VISTA)
        sizeof(BOOLEAN), /* Query support only on Vista and above */
#else
        0,
#endif
        sizeof(BOOLEAN), /* Query value is a BOOLEAN: 1-byte alignment */
        0, /* No size for Set */
        sizeof(BOOLEAN), /* Set takes no buffer; align to BOOLEAN so the
                          * length check (length != 0) fires instead of a
                          * spurious STATUS_DATATYPE_MISALIGNMENT */
#if (NTDDI_VERSION >= NTDDI_VISTA)
        ICIF_QUERY |
#endif
        ICIF_SET
    },

    /* ThreadBreakOnTermination */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),

    /* ThreadSwitchLegacyState */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_SET
    ),

    /* ThreadIsTerminated */
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY
    ),

    /* ThreadLastSystemCall (covered by probelib special-case) */
    IQS_NONE,

    /* ThreadIoPriority (covered by probelib special-case) */
    IQS_NONE,

    /* ThreadCycleTime (covered by probelib special-case) */
    IQS_NONE,

    /* ThreadPagePriority (covered by probelib special-case) */
    IQS_NONE,

    /* ThreadActualBasePriority (covered by probelib special-case) */
    IQS_NONE,

    /* ThreadTebInformation (covered by probelib special-case) */
    IQS_NONE,

    /* ThreadCSwitchMon -- no table entry on Windows */
    IQS_NONE,

    // Windows 7
    /* ThreadCSwitchPmu -- no table entry on Windows */
    IQS_NONE,
    /* ThreadWow64Context -- variable handling; left unvalidated (see probelib.c) */
    IQS_NONE,

    /* ThreadGroupInformation */
#if (NTDDI_VERSION >= NTDDI_WIN7)
    IQS_SAME
    (
        GROUP_AFFINITY,
        ULONGLONG,
        ICIF_QUERY | ICIF_SET
    ),
#else
    IQS_NONE,
#endif

    /* ThreadUmsInformation */
    IQS_NONE,
    /* ThreadCounterProfiling */
    IQS_NONE,

    /* ThreadIdealProcessorEx */
#if (NTDDI_VERSION >= NTDDI_WIN7)
    IQS_SAME
    (
        PROCESSOR_NUMBER,
        USHORT,
        ICIF_QUERY | ICIF_SET
    ),
#else
    IQS_NONE,
#endif

    // Windows 8
    /* ThreadCpuAccountingInformation */
    IQS_NONE,

    // Windows 8.1
    /* ThreadSuspendCount */
#if (NTDDI_VERSION >= NTDDI_WINBLUE)
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY
    ),
#else
    IQS_NONE,
#endif

    // Windows 10
    /* ThreadHeterogeneousCpuPolicy */
#if (NTDDI_VERSION >= NTDDI_WIN10)
    IQS_SAME
    (
        ULONG, // KHETERO_CPU_POLICY
        ULONG,
        ICIF_QUERY
    ),
#else
    IQS_NONE,
#endif

    /* ThreadContainerId */
#if (NTDDI_VERSION >= NTDDI_WIN10)
    IQS_SAME
    (
        GUID,
        ULONG, // GUID's natural alignment is that of its leading ULONG (Data1)
        ICIF_QUERY
    ),
#else
    IQS_NONE,
#endif

    /* ThreadNameInformation */
    IQS_SAME
    (
        UNICODE_STRING,
        ULONG_PTR,
        ICIF_QUERY | ICIF_SET | ICIF_SIZE_VARIABLE
    ),

    /* ThreadSelectedCpuSets (variable; covered by probelib special-case) */
    IQS_NONE,

    /* ThreadSystemThreadInformation (Win10+; SYSTEM_THREAD_INFORMATION, query-only) */
#if (NTDDI_VERSION >= NTDDI_WIN10)
    IQS_SAME
    (
        SYSTEM_THREAD_INFORMATION,
        ULONGLONG,
        ICIF_QUERY
    ),
#else
    IQS_NONE,
#endif

    /* ThreadActualGroupAffinity */
#if (NTDDI_VERSION >= NTDDI_WIN10)
    IQS_SAME
    (
        GROUP_AFFINITY,
        ULONGLONG,
        ICIF_QUERY
    ),
#else
    IQS_NONE,
#endif

    /* ThreadDynamicCodePolicyInfo */
#if (NTDDI_VERSION >= NTDDI_WIN10)
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),
#else
    IQS_NONE,
#endif

    /* ThreadExplicitCaseSensitivity */
#if (NTDDI_VERSION >= NTDDI_WIN10)
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),
#else
    IQS_NONE,
#endif

    /* ThreadWorkOnBehalfTicket (Win10+; ALPC_WORK_ON_BEHALF_TICKET, 8 bytes, query + set) */
#if (NTDDI_VERSION >= NTDDI_WIN10)
    IQS_SAME
    (
        ULONGLONG, // ALPC_WORK_ON_BEHALF_TICKET (2x ULONG)
        ULONG,
        ICIF_QUERY | ICIF_SET
    ),
#else
    IQS_NONE,
#endif

    /* ThreadSubsystemInformation (Win10+; SUBSYSTEM_INFORMATION_TYPE == ULONG, query-only) */
#if (NTDDI_VERSION >= NTDDI_WIN10)
    IQS_SAME
    (
        ULONG,
        ULONG,
        ICIF_QUERY
    ),
#else
    IQS_NONE,
#endif

    /* ThreadDbgkWerReportActive (Win10+; BOOLEAN, set-only) */
#if (NTDDI_VERSION >= NTDDI_WIN10)
    IQS
    (
        CHAR,
        CHAR,
        BOOLEAN,
        UCHAR,
        ICIF_SET
    ),
#else
    IQS_NONE,
#endif

    /* ThreadAttachContainer (Win10+; HANDLE, set-only) */
#if (NTDDI_VERSION >= NTDDI_WIN10)
    IQS_SAME
    (
        HANDLE,
        ULONGLONG,
        ICIF_SET
    ),
#else
    IQS_NONE,
#endif

    /* ThreadManageWritesToExecutableMemory (variable; covered by probelib special-case) */
    IQS_NONE,

    /* ThreadPowerThrottlingState (Win10 RS3+; THREAD_POWER_THROTTLING_STATE, 12 bytes, set-only) */
#if (NTDDI_VERSION >= NTDDI_WIN10_RS3)
    {
        0,
        sizeof(ULONG),
        3 * sizeof(ULONG), // THREAD_POWER_THROTTLING_STATE (Version, ControlMask, StateMask)
        sizeof(ULONG),
        ICIF_SET
    },
#else
    IQS_NONE,
#endif

    /* ThreadWorkloadClass (Win10 RS5+; THREAD_WORKLOAD_CLASS == ULONG, set-only) */
#if (NTDDI_VERSION >= NTDDI_WIN10_RS5)
    IQS_SAME
    (
        ULONG, // THREAD_WORKLOAD_CLASS
        ULONG,
        ICIF_SET
    ),
#else
    IQS_NONE,
#endif
    /* ThreadCreateStateChange */
    IQS_NONE,
    /* ThreadApplyStateChange */
    IQS_NONE,
    /* ThreadStrongerBadHandleChecks */
    IQS_NONE,
    /* ThreadEffectiveIoPriority */
    IQS_NONE,
    /* ThreadEffectivePagePriority */
    IQS_NONE,
};
