/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS Win32 Base API
 * FILE:            dll/win32/kernel32/kernel32_vista/threadpool.c
 * PURPOSE:         Vista+ Threadpool API wrappers around ntdll Tp* primitives
 *
 * Simple 1:1 ntdll forwarders (CloseThreadpool*, SetThreadpoolWait, etc.)
 * are declared in kernel32.spec. This file holds the variants that return
 * a handle via the function value and require last-error translation from
 * NTSTATUS, plus TrySubmitThreadpoolCallback.
 */

#include "k32_vista.h"

extern NTSTATUS WINAPI TpAllocPool(PTP_POOL *, PVOID);
extern NTSTATUS WINAPI TpAllocCleanupGroup(PTP_CLEANUP_GROUP *);
extern NTSTATUS WINAPI TpAllocIoCompletion(PTP_IO *, HANDLE, PVOID, PVOID, PTP_CALLBACK_ENVIRON);
extern NTSTATUS WINAPI TpAllocTimer(PTP_TIMER *, PTP_TIMER_CALLBACK, PVOID, PTP_CALLBACK_ENVIRON);
extern NTSTATUS WINAPI TpAllocWait(PTP_WAIT *, PTP_WAIT_CALLBACK, PVOID, PTP_CALLBACK_ENVIRON);
extern NTSTATUS WINAPI TpAllocWork(PTP_WORK *, PTP_WORK_CALLBACK, PVOID, PTP_CALLBACK_ENVIRON);
extern NTSTATUS WINAPI TpSimpleTryPost(PTP_SIMPLE_CALLBACK, PVOID, PTP_CALLBACK_ENVIRON);
extern NTSTATUS WINAPI TpSetPoolStackInformation(PTP_POOL, PTP_POOL_STACK_INFORMATION);
extern NTSTATUS WINAPI TpQueryPoolStackInformation(PTP_POOL, PTP_POOL_STACK_INFORMATION);

/***********************************************************************
 *           TrySubmitThreadpoolCallback   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK callback,
                                                          PVOID userdata,
                                                          TP_CALLBACK_ENVIRON *environment)
{
    NTSTATUS status = TpSimpleTryPost(callback, userdata, environment);
    if (!NT_SUCCESS(status))
    {
        SetLastError(RtlNtStatusToDosError(status));
        return FALSE;
    }
    return TRUE;
}

/***********************************************************************
 *           CreateThreadpool   (kernelbase.@)
 */
PTP_POOL WINAPI DECLSPEC_HOTPATCH CreateThreadpool(PVOID reserved)
{
    PTP_POOL pool = NULL;
    NTSTATUS status = TpAllocPool(&pool, reserved);
    if (!NT_SUCCESS(status))
    {
        SetLastError(RtlNtStatusToDosError(status));
        return NULL;
    }
    return pool;
}

/***********************************************************************
 *           CreateThreadpoolCleanupGroup   (kernelbase.@)
 */
PTP_CLEANUP_GROUP WINAPI DECLSPEC_HOTPATCH CreateThreadpoolCleanupGroup(VOID)
{
    PTP_CLEANUP_GROUP group = NULL;
    NTSTATUS status = TpAllocCleanupGroup(&group);
    if (!NT_SUCCESS(status))
    {
        SetLastError(RtlNtStatusToDosError(status));
        return NULL;
    }
    return group;
}

/***********************************************************************
 *           CreateThreadpoolIo   (kernelbase.@)
 */
PTP_IO WINAPI DECLSPEC_HOTPATCH CreateThreadpoolIo(HANDLE handle,
                                                   PTP_WIN32_IO_CALLBACK callback,
                                                   PVOID userdata,
                                                   TP_CALLBACK_ENVIRON *environment)
{
    PTP_IO io = NULL;
    NTSTATUS status = TpAllocIoCompletion(&io, handle, (PVOID)callback, userdata, environment);
    if (!NT_SUCCESS(status))
    {
        SetLastError(RtlNtStatusToDosError(status));
        return NULL;
    }
    return io;
}

/***********************************************************************
 *           CreateThreadpoolTimer   (kernelbase.@)
 */
PTP_TIMER WINAPI DECLSPEC_HOTPATCH CreateThreadpoolTimer(PTP_TIMER_CALLBACK callback,
                                                         PVOID userdata,
                                                         TP_CALLBACK_ENVIRON *environment)
{
    PTP_TIMER timer = NULL;
    NTSTATUS status = TpAllocTimer(&timer, callback, userdata, environment);
    if (!NT_SUCCESS(status))
    {
        SetLastError(RtlNtStatusToDosError(status));
        return NULL;
    }
    return timer;
}

/***********************************************************************
 *           CreateThreadpoolWait   (kernelbase.@)
 */
PTP_WAIT WINAPI DECLSPEC_HOTPATCH CreateThreadpoolWait(PTP_WAIT_CALLBACK callback,
                                                       PVOID userdata,
                                                       TP_CALLBACK_ENVIRON *environment)
{
    PTP_WAIT wait = NULL;
    NTSTATUS status = TpAllocWait(&wait, callback, userdata, environment);
    if (!NT_SUCCESS(status))
    {
        SetLastError(RtlNtStatusToDosError(status));
        return NULL;
    }
    return wait;
}

/***********************************************************************
 *           CreateThreadpoolWork   (kernelbase.@)
 */
PTP_WORK WINAPI DECLSPEC_HOTPATCH CreateThreadpoolWork(PTP_WORK_CALLBACK callback,
                                                       PVOID userdata,
                                                       TP_CALLBACK_ENVIRON *environment)
{
    PTP_WORK work = NULL;
    NTSTATUS status = TpAllocWork(&work, callback, userdata, environment);
    if (!NT_SUCCESS(status))
    {
        SetLastError(RtlNtStatusToDosError(status));
        return NULL;
    }
    return work;
}

/***********************************************************************
 *           SetThreadpoolStackInformation   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetThreadpoolStackInformation(PTP_POOL pool,
                                                            PTP_POOL_STACK_INFORMATION stack_info)
{
    NTSTATUS status = TpSetPoolStackInformation(pool, stack_info);
    if (!NT_SUCCESS(status))
    {
        SetLastError(RtlNtStatusToDosError(status));
        return FALSE;
    }
    return TRUE;
}

/***********************************************************************
 *           QueryThreadpoolStackInformation   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH QueryThreadpoolStackInformation(PTP_POOL pool,
                                                              PTP_POOL_STACK_INFORMATION stack_info)
{
    NTSTATUS status = TpQueryPoolStackInformation(pool, stack_info);
    if (!NT_SUCCESS(status))
    {
        SetLastError(RtlNtStatusToDosError(status));
        return FALSE;
    }
    return TRUE;
}
