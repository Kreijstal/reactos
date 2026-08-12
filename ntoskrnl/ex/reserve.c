/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL-2.0-or-later
 * PURPOSE:         Preallocated APC and I/O completion reserve objects
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define RESERVE_QUERY_STATE  0x0001
#define RESERVE_MODIFY_STATE 0x0002
#define RESERVE_ALL_ACCESS   (STANDARD_RIGHTS_REQUIRED | \
                              RESERVE_QUERY_STATE | RESERVE_MODIFY_STATE)

typedef struct _EX_USER_APC_RESERVE
{
    KAPC Apc;
    volatile LONG InUse;
} EX_USER_APC_RESERVE, *PEX_USER_APC_RESERVE;

typedef struct _EX_IO_COMPLETION_RESERVE
{
    IOP_MINI_COMPLETION_PACKET Packet;
    volatile LONG InUse;
} EX_IO_COMPLETION_RESERVE, *PEX_IO_COMPLETION_RESERVE;

static POBJECT_TYPE ExpUserApcReserveObjectType;
static POBJECT_TYPE ExpIoCompletionReserveObjectType;

CODE_SEG("INIT")
BOOLEAN
NTAPI
ExpInitializeReserveObjectImplementation(VOID)
{
    OBJECT_TYPE_INITIALIZER ObjectTypeInitializer;
    GENERIC_MAPPING Mapping =
    {
        STANDARD_RIGHTS_READ | RESERVE_QUERY_STATE,
        STANDARD_RIGHTS_WRITE | RESERVE_MODIFY_STATE,
        STANDARD_RIGHTS_EXECUTE,
        RESERVE_ALL_ACCESS
    };
    UNICODE_STRING Name;
    NTSTATUS Status;

    RtlZeroMemory(&ObjectTypeInitializer, sizeof(ObjectTypeInitializer));
    ObjectTypeInitializer.Length = sizeof(ObjectTypeInitializer);
    ObjectTypeInitializer.GenericMapping = Mapping;
    ObjectTypeInitializer.ValidAccessMask = RESERVE_ALL_ACCESS;
    ObjectTypeInitializer.PoolType = NonPagedPool;

    RtlInitUnicodeString(&Name, L"UserApcReserve");
    ObjectTypeInitializer.DefaultNonPagedPoolCharge = sizeof(EX_USER_APC_RESERVE);
    Status = ObCreateObjectType(&Name,
                                &ObjectTypeInitializer,
                                NULL,
                                &ExpUserApcReserveObjectType);
    if (!NT_SUCCESS(Status))
        return FALSE;

    RtlInitUnicodeString(&Name, L"IoCompletionReserve");
    ObjectTypeInitializer.DefaultNonPagedPoolCharge = sizeof(EX_IO_COMPLETION_RESERVE);
    Status = ObCreateObjectType(&Name,
                                &ObjectTypeInitializer,
                                NULL,
                                &ExpIoCompletionReserveObjectType);
    return NT_SUCCESS(Status);
}

NTSTATUS
NTAPI
ExpAcquireUserApcReserve(
    _In_ HANDLE ReserveHandle,
    _In_ KPROCESSOR_MODE AccessMode,
    _Out_ PKAPC *Apc)
{
    PEX_USER_APC_RESERVE Reserve;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(ReserveHandle,
                                       RESERVE_MODIFY_STATE,
                                       ExpUserApcReserveObjectType,
                                       AccessMode,
                                       (PVOID *)&Reserve,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    if (InterlockedCompareExchange(&Reserve->InUse, 1, 0) != 0)
    {
        ObDereferenceObject(Reserve);
        return STATUS_RESOURCE_IN_USE;
    }

    *Apc = &Reserve->Apc;
    return STATUS_SUCCESS;
}

VOID
NTAPI
ExpReleaseUserApcReserve(_In_ PKAPC Apc)
{
    PEX_USER_APC_RESERVE Reserve;

    Reserve = CONTAINING_RECORD(Apc, EX_USER_APC_RESERVE, Apc);
    InterlockedExchange(&Reserve->InUse, 0);
    ObDereferenceObject(Reserve);
}

NTSTATUS
NTAPI
ExpAcquireIoCompletionReserve(
    _In_ HANDLE ReserveHandle,
    _In_ KPROCESSOR_MODE AccessMode,
    _Out_ PVOID *Packet)
{
    PEX_IO_COMPLETION_RESERVE Reserve;
    NTSTATUS Status;

    Status = ObReferenceObjectByHandle(ReserveHandle,
                                       RESERVE_MODIFY_STATE,
                                       ExpIoCompletionReserveObjectType,
                                       AccessMode,
                                       (PVOID *)&Reserve,
                                       NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    if (InterlockedCompareExchange(&Reserve->InUse, 1, 0) != 0)
    {
        ObDereferenceObject(Reserve);
        return STATUS_RESOURCE_IN_USE;
    }

    Reserve->Packet.PacketType = IopCompletionPacketQuota;
    *Packet = &Reserve->Packet;
    return STATUS_SUCCESS;
}

VOID
NTAPI
ExpReleaseIoCompletionReserve(_In_ PVOID Packet)
{
    PEX_IO_COMPLETION_RESERVE Reserve;

    Reserve = CONTAINING_RECORD(Packet, EX_IO_COMPLETION_RESERVE, Packet);
    InterlockedExchange(&Reserve->InUse, 0);
    ObDereferenceObject(Reserve);
}

NTSTATUS
NTAPI
NtAllocateReserveObject(
    _Out_ PHANDLE MemoryReserveHandle,
    _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
    _In_ MEMORY_RESERVE_OBJECT_TYPE Type)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    POBJECT_TYPE ObjectType;
    SIZE_T ObjectSize;
    PVOID Reserve;
    HANDLE Handle;
    NTSTATUS Status;
    PAGED_CODE();

    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForWriteHandle(MemoryReserveHandle);
            *MemoryReserveHandle = NULL;
            if (ObjectAttributes)
            {
                ProbeForRead(ObjectAttributes,
                             sizeof(*ObjectAttributes),
                             TYPE_ALIGNMENT(OBJECT_ATTRIBUTES));
                if (ObjectAttributes->ObjectName)
                    _SEH2_YIELD(return STATUS_OBJECT_NAME_INVALID);
            }
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    if (Type == MemoryReserveObjectTypeUserApc)
    {
        ObjectType = ExpUserApcReserveObjectType;
        ObjectSize = sizeof(EX_USER_APC_RESERVE);
    }
    else if (Type == MemoryReserveObjectTypeIoCompletion)
    {
        ObjectType = ExpIoCompletionReserveObjectType;
        ObjectSize = sizeof(EX_IO_COMPLETION_RESERVE);
    }
    else
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((PreviousMode == KernelMode) &&
        ObjectAttributes && ObjectAttributes->ObjectName)
        return STATUS_OBJECT_NAME_INVALID;

    Status = ObCreateObject(PreviousMode,
                            ObjectType,
                            ObjectAttributes,
                            PreviousMode,
                            NULL,
                            ObjectSize,
                            0,
                            0,
                            &Reserve);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(Reserve, ObjectSize);
    Status = ObInsertObject(Reserve,
                            NULL,
                            RESERVE_ALL_ACCESS,
                            0,
                            NULL,
                            &Handle);
    if (!NT_SUCCESS(Status))
        return Status;

    _SEH2_TRY
    {
        *MemoryReserveHandle = Handle;
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        Status = _SEH2_GetExceptionCode();
        ObCloseHandle(Handle, PreviousMode);
    }
    _SEH2_END;

    return Status;
}
