/*
 * PROJECT:     ReactOS Native DLL
 * PURPOSE:     Extended user thread creation
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

typedef struct _NTDLL_THREAD_START_CONTEXT
{
    PTHREAD_START_ROUTINE StartRoutine;
    PVOID Argument;
} NTDLL_THREAD_START_CONTEXT, *PNTDLL_THREAD_START_CONTEXT;

static
ULONG
NTAPI
NtdllpThreadStart(_In_ PVOID Parameter)
{
    NTDLL_THREAD_START_CONTEXT Context;
    PVOID Allocation = Parameter;
    SIZE_T AllocationSize = 0;
    NTSTATUS ExitStatus;

    Context = *(PNTDLL_THREAD_START_CONTEXT)Parameter;
    NtFreeVirtualMemory(NtCurrentProcess(),
                        &Allocation,
                        &AllocationSize,
                        MEM_RELEASE);

    ExitStatus = (NTSTATUS)Context.StartRoutine(Context.Argument);
    RtlExitUserThread(ExitStatus);
    return ExitStatus;
}

NTSTATUS
NTAPI
NtCreateThreadEx(
    _Out_ PHANDLE ThreadHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
    _In_ HANDLE ProcessHandle,
    _In_ PTHREAD_START_ROUTINE StartRoutine,
    _In_opt_ PVOID Argument,
    _In_ ULONG CreateFlags,
    _In_ ULONG_PTR ZeroBits,
    _In_ SIZE_T StackSize,
    _In_ SIZE_T MaximumStackSize,
    _In_opt_ PPS_ATTRIBUTE_LIST AttributeList)
{
    PSECURITY_DESCRIPTOR SecurityDescriptor = NULL;
    ULONG HandleAttributes = 0;
    NTDLL_THREAD_START_CONTEXT StartContext;
    PVOID RemoteStartContext = NULL;
    SIZE_T ContextSize, BytesWritten;
    HANDLE InternalHandle, ReturnedHandle;
    NTSTATUS Status;

    if (!ThreadHandle || !StartRoutine || AttributeList || ZeroBits > MAXULONG)
        return STATUS_INVALID_PARAMETER;

    if (CreateFlags & ~(THREAD_CREATE_FLAGS_CREATE_SUSPENDED |
                        THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH |
                        THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (ObjectAttributes)
    {
        if (ObjectAttributes->Length != sizeof(*ObjectAttributes) ||
            ObjectAttributes->ObjectName)
        {
            return STATUS_INVALID_PARAMETER;
        }

        SecurityDescriptor = ObjectAttributes->SecurityDescriptor;
        HandleAttributes = ObjectAttributes->Attributes & OBJ_INHERIT;
    }

    *ThreadHandle = NULL;

    ContextSize = sizeof(StartContext);
    Status = NtAllocateVirtualMemory(ProcessHandle,
                                     &RemoteStartContext,
                                     0,
                                     &ContextSize,
                                     MEM_RESERVE | MEM_COMMIT,
                                     PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
        return Status;

    StartContext.StartRoutine = StartRoutine;
    StartContext.Argument = Argument;
    Status = NtWriteVirtualMemory(ProcessHandle,
                                  RemoteStartContext,
                                  &StartContext,
                                  sizeof(StartContext),
                                  &BytesWritten);
    if (!NT_SUCCESS(Status) || BytesWritten != sizeof(StartContext))
    {
        if (NT_SUCCESS(Status))
            Status = STATUS_PARTIAL_COPY;
        goto FreeStartContext;
    }

    /* Keep the thread stopped until flags and final handle access are set. */
    Status = RtlCreateUserThread(ProcessHandle,
                                 SecurityDescriptor,
                                 TRUE,
                                 (ULONG)ZeroBits,
                                 MaximumStackSize,
                                 StackSize,
                                 NtdllpThreadStart,
                                 RemoteStartContext,
                                 &InternalHandle,
                                 NULL);
    if (!NT_SUCCESS(Status))
        goto FreeStartContext;

    if (CreateFlags & THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER)
    {
        Status = NtSetInformationThread(InternalHandle,
                                        ThreadHideFromDebugger,
                                        NULL,
                                        0);
        if (!NT_SUCCESS(Status))
            goto Failure;
    }

    if ((DesiredAccess == THREAD_ALL_ACCESS) && !HandleAttributes)
    {
        ReturnedHandle = InternalHandle;
    }
    else
    {
        Status = NtDuplicateObject(NtCurrentProcess(),
                                   InternalHandle,
                                   NtCurrentProcess(),
                                   &ReturnedHandle,
                                   DesiredAccess,
                                   HandleAttributes,
                                   0);
        if (!NT_SUCCESS(Status))
            goto Failure;
    }

    if (!(CreateFlags & THREAD_CREATE_FLAGS_CREATE_SUSPENDED))
    {
        Status = NtResumeThread(InternalHandle, NULL);
        if (!NT_SUCCESS(Status))
        {
            NtClose(ReturnedHandle);
            goto Failure;
        }
    }

    if (ReturnedHandle != InternalHandle)
        NtClose(InternalHandle);
    *ThreadHandle = ReturnedHandle;
    return STATUS_SUCCESS;

Failure:
    NtTerminateThread(InternalHandle, Status);
    NtClose(InternalHandle);
FreeStartContext:
    ContextSize = 0;
    NtFreeVirtualMemory(ProcessHandle,
                        &RemoteStartContext,
                        &ContextSize,
                        MEM_RELEASE);
    return Status;
}
