/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS WoW64 CPU Support
 * FILE:            dll/wow64cpu/wow64cpu.c
 * PURPOSE:         WoW64 CPU abstraction layer (stub)
 * PROGRAMMER:      ReactOS Team
 */

#include <stdarg.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <ndk/ntndk.h>

#define NDEBUG
#include <debug.h>

BOOL
WINAPI
DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
    return TRUE;
}

NTSTATUS
WINAPI
CpuProcessInit(VOID)
{
    DPRINT1("wow64cpu: CpuProcessInit stub\n");
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
CpuProcessTerm(VOID)
{
    DPRINT1("wow64cpu: CpuProcessTerm stub\n");
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
CpuThreadInit(PVOID Context)
{
    DPRINT1("wow64cpu: CpuThreadInit stub\n");
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
CpuThreadTerm(VOID)
{
    DPRINT1("wow64cpu: CpuThreadTerm stub\n");
    return STATUS_SUCCESS;
}

VOID
WINAPI
CpuSimulate(VOID)
{
    DPRINT1("wow64cpu: CpuSimulate stub - not implemented!\n");
}

NTSTATUS
WINAPI
CpuGetContext(HANDLE Thread, PVOID Context, PVOID XStateContext, ULONG Flags)
{
    DPRINT1("wow64cpu: CpuGetContext stub\n");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
CpuSetContext(HANDLE Thread, PVOID Context, PVOID XStateContext, ULONG Flags)
{
    DPRINT1("wow64cpu: CpuSetContext stub\n");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
CpuFlushInstructionCache(PVOID BaseAddress, SIZE_T Length)
{
    DPRINT1("wow64cpu: CpuFlushInstructionCache stub\n");
    return STATUS_SUCCESS;
}

VOID
WINAPI
CpuNotifyDllLoad(LPCWSTR DllName, PVOID DllBase, ULONG DllSize)
{
}

VOID
WINAPI
CpuNotifyDllUnload(PVOID DllBase)
{
}

NTSTATUS
WINAPI
CpuInitializeStartupContext(PVOID Context, PVOID EntryPoint, PVOID StackBase, ULONG StackSize)
{
    DPRINT1("wow64cpu: CpuInitializeStartupContext stub\n");
    return STATUS_NOT_IMPLEMENTED;
}

ULONG_PTR
WINAPI
CpuGetStackPointer(VOID)
{
    DPRINT1("wow64cpu: CpuGetStackPointer stub\n");
    return 0;
}

NTSTATUS
WINAPI
CpuSetStackPointer(ULONG_PTR StackPointer)
{
    DPRINT1("wow64cpu: CpuSetStackPointer stub\n");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
CpuSetInstructionPointer(ULONG_PTR InstructionPointer)
{
    DPRINT1("wow64cpu: CpuSetInstructionPointer stub\n");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
CpuResetToConsistentState(PVOID ExceptionPointers)
{
    DPRINT1("wow64cpu: CpuResetToConsistentState stub\n");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
CpuResetFloatingPoint(VOID)
{
    DPRINT1("wow64cpu: CpuResetFloatingPoint stub\n");
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
CpuSuspendThread(HANDLE Thread, PULONG PreviousSuspendCount)
{
    return NtSuspendThread(Thread, PreviousSuspendCount);
}

NTSTATUS
WINAPI
CpuSuspendLocalThread(VOID)
{
    DPRINT1("wow64cpu: CpuSuspendLocalThread stub\n");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
CpuNotifyAffinityChange(ULONG_PTR AffinityMask)
{
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
CpuNotifyBeforeFork(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
CpuNotifyAfterFork(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
CpuProcessDebugEvent(PVOID DebugEvent)
{
    DPRINT1("wow64cpu: CpuProcessDebugEvent stub\n");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
WINAPI
CpuPrepareForDebuggerAttach(VOID)
{
    DPRINT1("wow64cpu: CpuPrepareForDebuggerAttach stub\n");
    return STATUS_NOT_IMPLEMENTED;
}
